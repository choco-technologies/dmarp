# On-target testing: does the responder actually answer?

`tests/dmarp_test.c` runs against fixture interfaces registered on
`"/dev/null"`. Those can never go up or carry a real frame, so the one thing
they cannot cover is the thing that matters most in the field: whether a
device running dmarp answers an ARP request from a real link partner, with
the right address, and stays quiet for addresses it does not own.

`tests/arp_probe.py` covers that. It runs on the Linux box the device is
cabled to and drives the whole exchange from the outside.

## What it checks

| Check | Why it is there |
|---|---|
| A reply arrives at all | The responder path (`dmarp_note_frame()` -> `answer_if_for_us()`) runs and the frame reaches the wire |
| Exactly one responder | A second, differing answer for the same address means two devices claim it |
| Sender MAC is the expected one | The reply advertises the interface's real address, not a stale or zeroed one |
| Ethernet source matches the ARP sender | A responder can disagree with itself; neither layer alone shows it |
| Reply is unicast to the requester | RFC 826 replies are unicast - a broadcast reply is a responder that did not read the request |
| Reply carries the requester's own hardware and protocol address | The reply answers *this* request rather than being a canned frame |
| **No reply for a foreign address** | Catches a responder that answers for everything, which poisons every neighbour's cache on the segment - and which no positive-only test can see |

It sends the ARP request itself over an `AF_PACKET` raw socket instead of
provoking one with `ping`. Three consequences worth knowing:

- **It changes nothing on the host.** ARP is a layer-2 protocol, so crafting
  the request directly means the host needs no address in the device's subnet.
  Nothing to configure first, nothing to clean up, nothing left behind if the
  run is interrupted.
- **The request is exactly what the test says it is** - not filtered through
  the kernel's own retry, caching and duplicate-address behaviour.
- **The negative check is possible at all.** Asking for an address the device
  does not own and requiring silence cannot be expressed by provoking traffic
  with `ping`.

## Prerequisites on the device

dmarp answers a request only when both of these hold - the probe cannot tell
the two apart from the outside, it just sees silence:

1. **The interface has an IPv4 address.** `answer_if_for_us()` returns early
   when `dmnetif_get_ip_address()` reports none, so an interface that is up
   but unaddressed never answers.
2. **Something is pumping RX.** `dmarp_note_frame()` is fed by
   `dmnetbridge_handle_netif_rx()`, which has to be running in its own thread
   for the interface - that is `networkd`'s job. With no pump, frames sit in
   the driver and dmarp never sees them.

On a dmod-boot system, from the shell:

```
ifconfig eth0 up
ip addr add 192.168.50.10/24 dev eth0
service start networkd@eth0
```

Check it took:

```
ifconfig eth0          # expect flags=<UP,RUNNING> and the inet address
service list           # expect networkd running
```

Order matters: bring the interface up *before* starting the pump. A pump on a
down interface has nothing to read, and while dmnetbridge sleeps rather than
spinning in that state, there is no reason to make it wait.

## Running it

```bash
sudo ./tests/arp_probe.py --iface enp114s0 \
                         --target-ip 192.168.50.10 \
                         --expect-mac 02:00:00:00:00:01
```

`--iface` is the Linux interface the device is cabled to. If you are not sure
which one it is, look for the one that has carrier but no address, and whose
speed matches the device's PHY:

```bash
for i in /sys/class/net/*; do
    echo "$(basename $i): carrier=$(cat $i/carrier 2>/dev/null) speed=$(cat $i/speed 2>/dev/null)"
done
```

Useful options:

- `--expect-mac` - omit it and the reply's MAC is reported but not enforced.
  Pass it in CI, where "it answered with *something*" is not the assertion you
  want.
- `--timeout` / `--attempts` - default 2 s and 3 attempts. Raise them on a
  device that is slow to bring its link up.
- `--sender-ip` - the protocol address the request claims to come from.
  Defaults to the interface's own address if it has one, otherwise an address
  in the device's /24. The probe does not need to own it.
- `--no-negative` - skip the foreign-address check. Only useful on a segment
  with other hosts that legitimately answer for the probed address.

Exit status is 0 when every check passed, 1 when one did not, 2 for a usage or
environment problem - so it drops into a CI job or a shell `&&` chain as is.

## Self-test

```bash
./tests/arp_probe.py --self-test
```

This exercises the probe's own frame building and reply validation against
synthetic bytes. It needs no device, no interface and no privileges, so it
belongs in CI: a mistake in the probe's parsing then shows up as a failing
build rather than as a confusing result on someone's desk.

## When it fails

**No reply at all.** Work outwards from the device:

- `ifconfig <iface>` on the device - is it `UP` *and* does it have an `inet`
  address? Without the address dmarp will not answer (see Prerequisites).
- `service list` - is the pump running?
- `ifconfig <iface>` again after a probe run - did `RX packets` go up? If not,
  the request never reached the driver, and the problem is below dmarp:
  cabling, PHY, MAC address filtering (a broadcast ARP request needs the
  broadcast filter left enabled), or the RX descriptor path.
- If `RX packets` does go up but `TX packets` does not, the frame reached
  dmarp and was not answered - check that the address you probed is the one
  actually configured on the interface.

**A reply, but the wrong MAC.** The interface is answering with an address
other than the one you expected - check what `ifconfig` reports for `ether`,
and whether something reprogrammed it after startup.

**"no reply for a foreign address" fails.** The device answered for an address
it does not own. That is a responder bug, not a configuration problem, and it
is worth fixing before the device goes on a shared segment.

**More than one responder.** Something else on the segment claims the same
address. Disconnect the device and re-run: if a reply still arrives, the other
claimant is what you have been testing all along.
