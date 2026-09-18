#!/usr/bin/env python3
"""Host-side probe: does a device on this link answer ARP requests correctly?

dmarp's responder (``dmarp_note_frame()`` -> ``answer_if_for_us()``) only
proves itself against a real link partner, so this runs on the Linux box the
device is cabled to and drives the exchange from the outside.

It builds and sends the ARP request itself over an ``AF_PACKET`` raw socket
rather than provoking one with ``ping``. That matters for three reasons:

* **No host network configuration is touched.** ARP is a layer-2 protocol, so
  crafting the request directly means this box needs no address in the target's
  subnet - nothing to add before the test, nothing to clean up after it, and
  nothing left behind if the test is interrupted.
* **The request is exactly what the test says it is.** Letting the kernel emit
  it means testing the kernel's retry, caching and duplicate-address behaviour
  as much as the device's.
* **Negative cases become possible.** Asking for an address the device does
  *not* own and requiring silence is the check that catches a responder
  answering for everything - which poisons every neighbour's cache on the
  segment and is invisible to any test that only ever asks for the right
  address.

Usage (needs CAP_NET_RAW, so normally root):

    sudo ./arp_probe.py --iface enp114s0 --target-ip 192.168.50.10 \\
                        --expect-mac 02:00:00:00:00:01

    ./arp_probe.py --self-test     # frame build/parse checks only, no root

Exit status: 0 all checks passed, 1 a check failed, 2 bad usage/environment.
"""

import argparse
import fcntl
import os
import socket
import struct
import sys
import time

ETH_P_ARP = 0x0806
ETH_HEADER_LEN = 14
ARP_PAYLOAD_LEN = 28
ARP_FRAME_LEN = ETH_HEADER_LEN + ARP_PAYLOAD_LEN

ARP_HTYPE_ETHERNET = 1
ARP_PTYPE_IPV4 = 0x0800
ARP_OP_REQUEST = 1
ARP_OP_REPLY = 2

BROADCAST_MAC = b"\xff" * 6
ZERO_MAC = b"\x00" * 6

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2


# --------------------------------------------------------------------------
# Frame building and parsing
#
# Kept free of any socket or interface state so --self-test can exercise them
# anywhere, including CI containers with no network and no privileges.
# --------------------------------------------------------------------------

def mac_to_bytes(text):
    """Parse "aa:bb:cc:dd:ee:ff" into 6 raw bytes."""
    parts = text.split(":")
    if len(parts) != 6:
        raise ValueError("MAC address must have 6 colon-separated octets: %r" % text)
    try:
        return bytes(int(p, 16) for p in parts)
    except ValueError:
        raise ValueError("MAC address has a non-hexadecimal octet: %r" % text)


def mac_to_text(raw):
    return ":".join("%02x" % b for b in raw)


def ipv4_to_bytes(text):
    try:
        return socket.inet_aton(text)
    except OSError:
        raise ValueError("not an IPv4 address: %r" % text)


def build_arp_request(sender_mac, sender_ip, target_ip):
    """Build a complete broadcast ARP request frame.

    Deliberately emitted at exactly ARP_FRAME_LEN (42) bytes rather than
    padded to the 60-byte Ethernet minimum: the NIC pads on transmit, and
    sending the unpadded frame is what lets a receiver's own padding
    behaviour show up in what comes back.
    """
    eth = BROADCAST_MAC + sender_mac + struct.pack("!H", ETH_P_ARP)
    arp = struct.pack(
        "!HHBBH6s4s6s4s",
        ARP_HTYPE_ETHERNET,
        ARP_PTYPE_IPV4,
        6,
        4,
        ARP_OP_REQUEST,
        sender_mac,
        sender_ip,
        ZERO_MAC,
        target_ip,
    )
    return eth + arp


class ArpFrame(object):
    """The fields of a parsed ARP frame, plus the Ethernet header's own.

    Both layers are kept because a responder can disagree with itself - an
    Ethernet source address that differs from the ARP sender hardware address
    is a real bug that neither layer alone reveals.
    """

    def __init__(self, eth_dst, eth_src, opcode, sender_mac, sender_ip, target_mac, target_ip, length):
        self.eth_dst = eth_dst
        self.eth_src = eth_src
        self.opcode = opcode
        self.sender_mac = sender_mac
        self.sender_ip = sender_ip
        self.target_mac = target_mac
        self.target_ip = target_ip
        self.length = length


def parse_arp_frame(frame):
    """Parse an ARP frame, or return None if it is not one we understand.

    Returns None rather than raising: this is fed every frame the socket
    hands over, most of which are someone else's traffic.
    """
    if len(frame) < ARP_FRAME_LEN:
        return None
    if struct.unpack("!H", frame[12:14])[0] != ETH_P_ARP:
        return None

    arp = frame[ETH_HEADER_LEN:ARP_FRAME_LEN]
    htype, ptype, hlen, plen, opcode, smac, sip, tmac, tip = struct.unpack("!HHBBH6s4s6s4s", arp)
    if htype != ARP_HTYPE_ETHERNET or ptype != ARP_PTYPE_IPV4 or hlen != 6 or plen != 4:
        return None

    return ArpFrame(frame[0:6], frame[6:12], opcode, smac, sip, tmac, tip, len(frame))


# --------------------------------------------------------------------------
# Interface helpers
# --------------------------------------------------------------------------

def read_interface_mac(iface):
    path = "/sys/class/net/%s/address" % iface
    try:
        with open(path) as handle:
            return mac_to_bytes(handle.read().strip())
    except FileNotFoundError:
        raise ValueError("no such interface: %s" % iface)


def read_interface_ipv4(iface):
    """This interface's first IPv4 address, or None if it has none.

    SIOCGIFADDR rather than parsing `ip addr`, so the probe does not depend on
    iproute2 being installed or on its output format.
    """
    SIOCGIFADDR = 0x8915
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        packed = fcntl.ioctl(sock.fileno(), SIOCGIFADDR,
                             struct.pack("256s", iface.encode()[:15]))
        return packed[20:24]
    except OSError:
        return None
    finally:
        sock.close()


def default_sender_ip(iface, target_ip_bytes):
    """Pick a sender protocol address for the request.

    This interface's own address when it has one. Otherwise an address in the
    target's /24 - the probe does not need to own it (nothing replies *to* the
    request's sender address here), it just has to be a plausible one that is
    not the target's own. 0.0.0.0 would also be answered by a correct
    responder, but it makes the request an RFC 5227 ARP Probe, which some
    stacks treat as a duplicate-address check, and dmarp would cache
    "0.0.0.0 -> our MAC" from it - pointless state to leave behind on a device
    under test.
    """
    own = read_interface_ipv4(iface)
    if own is not None:
        return own

    last = target_ip_bytes[3]
    spare = 253 if last == 254 else 254
    return target_ip_bytes[0:3] + bytes([spare])


def unused_ip_in_subnet(target_ip_bytes):
    """An address in the target's /24 that the target itself does not own.

    Used for the negative check. .251/.250 is as arbitrary as any other choice;
    what matters is only that it differs from the target.
    """
    last = target_ip_bytes[3]
    spare = 250 if last == 251 else 251
    return target_ip_bytes[0:3] + bytes([spare])


# --------------------------------------------------------------------------
# The exchange
# --------------------------------------------------------------------------

def open_socket(iface):
    try:
        sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ARP))
    except PermissionError:
        raise ValueError("opening a raw socket needs CAP_NET_RAW - run this as root")
    sock.bind((iface, ETH_P_ARP))
    return sock


def request_and_collect(sock, request, target_ip, timeout_s):
    """Send `request`, then return every ARP reply for `target_ip` seen before
    the deadline, along with how long the first one took.

    Every reply is collected rather than just the first, because a second,
    differing answer for the same address is itself a finding - and the caller
    cannot see it if this stops at the first match.
    """
    sent_at = time.monotonic()
    sock.send(request)

    deadline = sent_at + timeout_s
    replies = []
    first_rtt_ms = None

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        sock.settimeout(remaining)
        try:
            frame = sock.recv(2048)
        except socket.timeout:
            break

        parsed = parse_arp_frame(frame)
        if parsed is None or parsed.opcode != ARP_OP_REPLY:
            continue
        if parsed.sender_ip != target_ip:
            continue

        if first_rtt_ms is None:
            first_rtt_ms = (time.monotonic() - sent_at) * 1000.0
        replies.append(parsed)

    return replies, first_rtt_ms


class Report(object):
    """Collects each check's outcome so the run prints one verdict at the end
    rather than stopping at the first problem - a failing MAC and a broadcast
    reply are separate facts, and seeing both at once is what makes the
    difference between one debugging round and two."""

    def __init__(self):
        self.failures = []

    def check(self, passed, name, detail=""):
        print("  [%s] %s%s" % ("PASS" if passed else "FAIL", name,
                               (" - " + detail) if detail else ""))
        if not passed:
            self.failures.append(name)
        return passed

    def note(self, text):
        print("  ....  %s" % text)


def probe(args):
    try:
        iface_mac = read_interface_mac(args.iface)
        target_ip = ipv4_to_bytes(args.target_ip)
        expect_mac = mac_to_bytes(args.expect_mac) if args.expect_mac else None
        sender_ip = (ipv4_to_bytes(args.sender_ip) if args.sender_ip
                     else default_sender_ip(args.iface, target_ip))
        sock = open_socket(args.iface)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE

    report = Report()
    print("Probing %s on %s (our MAC %s, asking as %s)" %
          (args.target_ip, args.iface, mac_to_text(iface_mac),
           socket.inet_ntoa(sender_ip)))

    try:
        # ---- Positive: the device answers for the address it owns ----
        print("\nARP request for %s:" % args.target_ip)
        request = build_arp_request(iface_mac, sender_ip, target_ip)

        replies = []
        rtt_ms = None
        for attempt in range(1, args.attempts + 1):
            replies, rtt_ms = request_and_collect(sock, request, target_ip, args.timeout)
            if replies:
                if attempt > 1:
                    report.note("answered on attempt %d of %d" % (attempt, args.attempts))
                break

        if not report.check(bool(replies), "reply received",
                            "no answer in %.1fs x %d attempt(s)" % (args.timeout, args.attempts)
                            if not replies else "%.1f ms" % rtt_ms):
            return EXIT_FAIL

        reply = replies[0]

        report.check(len(replies) == 1, "exactly one responder",
                     "%d replies for the same address" % len(replies) if len(replies) != 1 else "")

        if expect_mac is not None:
            report.check(reply.sender_mac == expect_mac, "sender MAC as expected",
                         "got %s, expected %s" % (mac_to_text(reply.sender_mac),
                                                  mac_to_text(expect_mac)))
        else:
            report.note("sender MAC %s (not checked - pass --expect-mac to enforce)"
                        % mac_to_text(reply.sender_mac))

        report.check(reply.eth_src == reply.sender_mac,
                     "Ethernet source matches ARP sender",
                     "Ethernet %s vs ARP %s" % (mac_to_text(reply.eth_src),
                                                mac_to_text(reply.sender_mac)))

        report.check(reply.eth_dst == iface_mac, "reply is unicast to the requester",
                     "addressed to %s" % mac_to_text(reply.eth_dst))

        report.check(reply.target_mac == iface_mac,
                     "reply carries the requester's hardware address",
                     "got %s, expected %s" % (mac_to_text(reply.target_mac),
                                              mac_to_text(iface_mac)))

        report.check(reply.target_ip == sender_ip,
                     "reply carries the requester's protocol address",
                     "got %s, expected %s" % (socket.inet_ntoa(reply.target_ip),
                                              socket.inet_ntoa(sender_ip)))

        report.note("reply frame was %d bytes on the wire" % reply.length)

        # ---- Negative: the device stays quiet for an address it does not own ----
        if args.negative:
            other_ip = unused_ip_in_subnet(target_ip)
            print("\nARP request for %s (address the device must NOT claim):"
                  % socket.inet_ntoa(other_ip))
            other_request = build_arp_request(iface_mac, sender_ip, other_ip)
            stray, _ = request_and_collect(sock, other_request, other_ip, args.timeout)

            impostors = [r for r in stray if expect_mac is None or r.sender_mac == expect_mac]
            report.check(not impostors, "no reply for a foreign address",
                         "the device answered for %s" % socket.inet_ntoa(other_ip)
                         if impostors else "")
            if stray and not impostors:
                report.note("%d reply/replies from other hosts on the segment, ignored"
                            % len(stray))
    finally:
        sock.close()

    print("")
    if report.failures:
        print("FAIL: %d check(s) failed: %s" % (len(report.failures), ", ".join(report.failures)))
        return EXIT_FAIL

    print("PASS: %s answers ARP correctly" % args.target_ip)
    return EXIT_OK


# --------------------------------------------------------------------------
# Self-test
#
# Exercises the frame building and the reply validation against synthetic
# bytes, with no interface, no privileges and no device - so CI can run it and
# a mistake in the parsing shows up as a failing build rather than as a
# confusing result on someone's desk.
# --------------------------------------------------------------------------

def self_test():
    failures = []

    def check(passed, name):
        print("  [%s] %s" % ("PASS" if passed else "FAIL", name))
        if not passed:
            failures.append(name)

    our_mac = mac_to_bytes("80:fa:5b:79:87:14")
    dev_mac = mac_to_bytes("02:00:00:00:00:01")
    our_ip = ipv4_to_bytes("192.168.50.1")
    dev_ip = ipv4_to_bytes("192.168.50.10")

    print("Frame building:")
    request = build_arp_request(our_mac, our_ip, dev_ip)
    check(len(request) == ARP_FRAME_LEN, "request is %d bytes" % ARP_FRAME_LEN)
    parsed = parse_arp_frame(request)
    check(parsed is not None, "request parses back")
    check(parsed.eth_dst == BROADCAST_MAC, "request is broadcast")
    check(parsed.opcode == ARP_OP_REQUEST, "request opcode is 1")
    check(parsed.sender_mac == our_mac and parsed.sender_ip == our_ip, "request carries our address")
    check(parsed.target_ip == dev_ip, "request asks for the target address")
    check(parsed.target_mac == ZERO_MAC, "request leaves target hardware address unset")

    print("\nReply parsing:")
    reply = (our_mac + dev_mac + struct.pack("!H", ETH_P_ARP) +
             struct.pack("!HHBBH6s4s6s4s", ARP_HTYPE_ETHERNET, ARP_PTYPE_IPV4, 6, 4,
                         ARP_OP_REPLY, dev_mac, dev_ip, our_mac, our_ip))
    parsed = parse_arp_frame(reply)
    check(parsed is not None, "reply parses")
    check(parsed.opcode == ARP_OP_REPLY, "reply opcode is 2")
    check(parsed.sender_mac == dev_mac and parsed.sender_ip == dev_ip, "reply carries device address")
    check(parsed.eth_dst == our_mac, "reply is unicast to us")

    # A 60-byte frame is what a real NIC puts on the wire, since it pads the
    # 42-byte ARP frame up to the Ethernet minimum. Parsing must ignore that
    # padding rather than trip over the extra bytes.
    check(parse_arp_frame(reply + b"\x00" * 18) is not None, "padded (60-byte) reply parses")

    print("\nRejection of frames that are not ARP replies we asked about:")
    check(parse_arp_frame(b"\x00" * 20) is None, "short frame rejected")
    not_arp = bytearray(reply)
    not_arp[12:14] = struct.pack("!H", 0x0800)
    check(parse_arp_frame(bytes(not_arp)) is None, "non-ARP ethertype rejected")
    bad_htype = bytearray(reply)
    bad_htype[14:16] = struct.pack("!H", 99)
    check(parse_arp_frame(bytes(bad_htype)) is None, "unknown hardware type rejected")

    print("\nAddress helpers:")
    check(default_sender_ip("definitely-not-an-interface", dev_ip) ==
          ipv4_to_bytes("192.168.50.254"), "sender address falls back into the target's /24")
    check(unused_ip_in_subnet(dev_ip) != dev_ip, "negative-case address differs from the target")
    check(unused_ip_in_subnet(ipv4_to_bytes("10.0.0.251")) == ipv4_to_bytes("10.0.0.250"),
          "negative-case address avoids colliding with the target")

    print("")
    if failures:
        print("FAIL: %d self-test check(s) failed: %s" % (len(failures), ", ".join(failures)))
        return EXIT_FAIL
    print("PASS: self-test")
    return EXIT_OK


def main():
    parser = argparse.ArgumentParser(
        description="Check that a device on this link answers ARP requests correctly.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Needs CAP_NET_RAW (normally root) except in --self-test mode.")
    parser.add_argument("--iface", help="Linux interface the device is cabled to, e.g. enp114s0")
    parser.add_argument("--target-ip", help="IPv4 address configured on the device under test")
    parser.add_argument("--expect-mac", help="MAC the device must answer with; checked when given")
    parser.add_argument("--sender-ip",
                        help="protocol address to send the request from "
                             "(default: this interface's own, else one in the target's /24)")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="seconds to wait for a reply (default: 2)")
    parser.add_argument("--attempts", type=int, default=3,
                        help="how many requests to send before giving up (default: 3)")
    parser.add_argument("--no-negative", dest="negative", action="store_false",
                        help="skip the check that the device stays quiet for a foreign address")
    parser.add_argument("--self-test", action="store_true",
                        help="check this script's own frame handling; no root or device needed")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if not args.iface or not args.target_ip:
        parser.error("--iface and --target-ip are required (or use --self-test)")

    if os.geteuid() != 0:
        print("error: a raw socket needs CAP_NET_RAW - rerun with sudo", file=sys.stderr)
        return EXIT_USAGE

    return probe(args)


if __name__ == "__main__":
    sys.exit(main())
