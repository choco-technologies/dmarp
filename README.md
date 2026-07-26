# dmarp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmarp is a [DMOD](https://github.com/choco-technologies/dmod) library
module that resolves an IPv4 address to a MAC address on a
directly-connected link, using ARP (RFC 826) request/reply frames - the
piece that turns "send to 192.168.1.1" into an actual destination MAC for
an Ethernet header, without every caller having to hand-roll ARP itself.
It sits directly on top of [dmnetif](https://github.com/choco-technologies/dmnetif)
(`dmnetif_send()` to transmit a request, `dmnetif_get_mac_address()`/
`_get_ip_address()` to build one) and keeps a cache so repeated
resolutions of the same destination don't send a fresh request every time.

```
┌──────────────────────────────────────────────┐
│                DMNETBRIDGE                    │
│   (routing/ARP/frame I/O for dmip, driven     │
│    by networkd)                               │
├──────────────────────────────────────────────┤
│                  DMARP                        │
│   resolve (cache + request/reply exchange),   │
│   cache lookup/insert/remove/count            │
├──────────────────────────────────────────────┤
│                  DMNETIF                      │
│   named interfaces, send/receive one frame    │
├──────────────────────────────────────────────┤
│                  DMROUTE                      │
│   shared dmroute_addr_t type                  │
└──────────────────────────────────────────────┘
```

IPv6 is out of scope: IPv6 neighbor discovery uses NDP over ICMPv6, an
entirely different protocol, not ARP - every dmarp function only ever
deals with `dmroute_family_v4` addresses.

## Key design points

- **`dmarp_resolve()` does not read frames off the wire itself.** Once
  `networkd` is running, `dmnetbridge_handle_netif_rx()` is the only code
  path allowed to call `dmnetif_receive()` on a given interface - a second
  concurrent reader would race it and starve. Instead, `dmarp_resolve()`
  sends its request and then waits on a signal that `dmarp_note_frame()`
  raises whenever *any* ARP frame comes in through dmnetbridge, re-checking
  the cache each time it wakes.
- **Opportunistic learning, not just replies to our own requests.**
  `dmarp_note_frame()` is fed *every* frame dmnetbridge's RX pump reads off
  an interface. Any well-formed ARP request or reply - not only a reply to
  a request we sent - has its sender's `(interface, IP) -> MAC` mapping
  cached unconditionally. That's what lets repeated sends to a peer we've
  merely *heard from* (e.g. its own broadcast ARP to someone else) skip a
  fresh round trip entirely.
- **Only one request per `dmarp_resolve()` call - no built-in retry.** A
  caller that wants retry behavior just calls `dmarp_resolve()` again after
  a timeout; the second call is its own independent request/wait cycle.
- **The cache is keyed by interface *name*, not by `dmnetif_iface_t`
  handle** - the same reasoning dmroute uses for its own routes: a stored
  handle could go stale across interface churn (unregister/re-register), a
  name can't. This does mean a re-registered interface with the same name
  inherits whatever was cached for that name before.
- **Every cache entry expires after `DMARP_CACHE_TTL_MS`** (60 seconds),
  applied uniformly whether the entry came from a real ARP reply or a
  manual `dmarp_cache_insert()` - there is no separate "static/permanent"
  entry concept. A hardcoded mapping needs to be re-inserted periodically
  to stay valid, same as anything else in the cache.
- **A resolving interface with no IP address of its own yet is not an
  error.** `dmarp_resolve()` still sends a request with an all-zero sender
  protocol address - a real host can legitimately ARP before it has an
  address of its own (e.g. during DHCP).

See [docs/dmarp.md](docs/dmarp.md) for the full rationale and
[docs/api-reference.md](docs/api-reference.md) for the complete API.

## Building

dmarp is a standalone DMOD module: its `CMakeLists.txt` fetches `dmod`
itself via CMake's `FetchContent` (defaulting to the `develop` branch), so
no other repository needs to be checked out first.

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This builds both the `dmarp` library module and its `test_dmarp` test
binary (see [Testing](#testing) below). Pass `-DDMOD_DIR=/path/to/local/dmod`
to build against a local dmod checkout instead of fetching it from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

The Makefile requires an existing `dmod` checkout (there is no
FetchContent equivalent for Make) and builds the `dmarp` module itself,
not the test suite.

## Testing

Tests are a plain DMOD test module built with `dmod_add_test()`:
`tests/dmarp_test.c` registers each test case with `DMOD_TEST_STEP()` and
runs against two fixture interfaces (`"test0"`/`"test1"`) registered
against `"/dev/null"` - real enough for the underlying file open to succeed
without needing an actual driver behind it (same pattern
[dmnetif](https://github.com/choco-technologies/dmnetif)'s own tests use).

After building (see above), run the resulting binary directly from the
build directory:

```bash
./tests/test_dmarp
```

It discovers and runs every `DMOD_TEST_STEP()` automatically and exits
with a code equal to the number of failed steps (`0` means everything
passed). Alternatively, it can be run through the real dmod loader, the
way CI does it - `dmf-get install` first resolves `test_dmarp`'s own
dependency closure (`dmnetif`, `dmroute`, ...) into `DMOD_DMF_DIR` so the
loader can find every module `test_dmarp.dmf` needs at load time:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmarp-local.dmd -y
dmod_loader build/dmf/test_dmarp.dmf
```

`tests/dmarp_test.c` covers:

- **Cache** - lookup on an unknown entry, insert/lookup round-trip, insert
  replacing an existing entry without growing the cache, independent
  entries for the same IP on two different interfaces, remove (present and
  absent entry), and invalid-argument (`NULL`/wrong family) rejection for
  every cache function.
- **`dmarp_resolve()`** - the cache-hit path (returns immediately, no
  frame sent), `NULL`/wrong-family argument rejection, and the
  `-ENODEV` path when the interface has no real driver behind it.
- **`dmarp_note_frame()`** - caching the sender from both a request and a
  reply, waking a cache lookup so a pending `dmarp_resolve()` finds the
  entry, ignoring a non-ARP ethertype, ignoring a too-short frame, and
  `NULL`-argument safety.

Since the fixture interfaces have no real driver behind them, they can
never actually go "up" or send/receive a real frame - `dmarp_resolve()`'s
reply-wait/timeout path itself is only exercised indirectly (through
`dmarp_note_frame()`'s cache side effect); real end-to-end coverage needs a
real network driver behind `dmnetif_register()`.

## Usage

### Resolving a destination before sending a frame

The common case: resolve the next-hop IP to a MAC address, then build the
Ethernet frame around it.

```c
#include "dmarp.h"
#include "dmnetif.h"

int send_to(dmnetif_iface_t iface, const dmroute_addr_t* next_hop,
            const void* payload, size_t payload_len)
{
    dmnetif_mac_addr_t dst_mac;
    int ret = dmarp_resolve(iface, next_hop, &dst_mac, DMARP_DEFAULT_TIMEOUT_MS);
    if (ret != 0)
    {
        return ret; /* -EINVAL / -ENODEV / -EIO / -ETIMEDOUT */
    }

    /* ... build an Ethernet frame addressed to dst_mac and dmnetif_send() it ... */
    return 0;
}
```

A cache hit returns immediately without sending anything; a cache miss
sends one ARP request and blocks up to `timeout_ms` waiting for a reply to
arrive through `dmarp_note_frame()` (see [Key design points](#key-design-points)).

### Checking the cache without ever blocking

Use `dmarp_cache_lookup()` when you want to know "do we already know this
destination?" without risking a send or a wait - e.g. to decide whether to
show a destination as "reachable" in a UI, or to skip work entirely when
nothing is cached yet.

```c
#include "dmarp.h"

dmnetif_mac_addr_t mac;
if (dmarp_cache_lookup(iface, &dst_ip, &mac))
{
    /* already known - use mac directly */
}
else
{
    /* not cached (or expired) - fall back to dmarp_resolve() if blocking is OK */
}
```

### Seeding a known mapping by hand

`dmarp_cache_insert()` is what `dmarp_resolve()` calls internally after a
successful reply, but it's also useful directly - for example to hardcode
a gateway's MAC address at startup so the very first packet to it doesn't
pay for an ARP round trip, or to set up a fixture in a test.

```c
#include "dmarp.h"

dmroute_addr_t gateway_ip = { .family = dmroute_family_v4, .addr.v4 = { 192, 168, 1, 1 } };
dmnetif_mac_addr_t gateway_mac = { { 0x00, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e } };

dmarp_cache_insert(iface, &gateway_ip, &gateway_mac);
```

Like any other entry, a manually-seeded one expires after
`DMARP_CACHE_TTL_MS` and needs to be re-inserted (or left to be re-resolved
normally) to stay valid.

### Invalidating a stale mapping

If something detects an address conflict (e.g. a duplicate-IP notification,
or a peer that stopped responding on its previously-known MAC),
`dmarp_cache_remove()` forces the next `dmarp_resolve()` for that
destination to send a fresh request instead of waiting out the TTL.

```c
#include "dmarp.h"

dmarp_cache_remove(iface, &suspect_ip);
```

### Wiring dmarp into a frame-receive pump

`dmarp_note_frame()` is meant to be called for *every* frame read off an
interface, not just ones a pending `dmarp_resolve()` is waiting on - this
is how [dmnetbridge](https://github.com/choco-technologies/dmnetbridge)'s
per-interface RX pump feeds it:

```c
#include "dmarp.h"
#include "dmnetif.h"

void handle_netif_rx(dmnetif_iface_t iface)
{
    uint8_t frame[2048]; /* large enough for one Ethernet frame */

    while (dmnetif_is_present(iface))
    {
        size_t frame_len = dmnetif_receive(iface, frame, sizeof(frame));
        if (frame_len == 0)
            continue;

        dmarp_note_frame(iface, frame, frame_len); /* opportunistic ARP learning */
        /* ... hand frame off to whatever else needs to see it (IP stack, etc.) ... */
    }
}
```

Frames that aren't a well-formed ARP request or reply are silently
ignored, so this is safe to call unconditionally on every received frame
regardless of its actual ethertype.

## API Overview

### Constants

| Constant                   | Value | Description                                                                |
|------------------------------|-------|-----------------------------------------------------------------------------|
| `DMARP_CACHE_TTL_MS`        | 60000 | How long a cache entry stays valid, however it got there.                 |
| `DMARP_DEFAULT_TIMEOUT_MS`  | 1000  | Suggested `timeout_ms` for `dmarp_resolve()` when you don't have a strong opinion. |

### Resolution

| Function                                             | Description                                                                  |
|---------------------------------------------------------|---------------------------------------------------------------------------------|
| `dmarp_resolve(iface, ip, mac, timeout_ms)`             | Resolve `ip` to a MAC address on `iface`. Cache hit: immediate. Cache miss: sends one ARP request, then waits up to `timeout_ms` for a reply. |
| `dmarp_note_frame(iface, frame, frame_len)`             | Feed one received frame to dmarp for opportunistic ARP learning and to wake any pending `dmarp_resolve()` call. |

### Cache

| Function                                       | Description                                                                  |
|-----------------------------------------------------|---------------------------------------------------------------------------------|
| `dmarp_cache_lookup(iface, ip, mac)`               | Look up a cached entry without ever sending a request or blocking.            |
| `dmarp_cache_insert(iface, ip, mac)`               | Add or replace a cache entry - usable directly to seed a known mapping.       |
| `dmarp_cache_remove(iface, ip)`                    | Remove one entry, if present. Safe when no matching entry exists.            |
| `dmarp_cache_count()`                              | Number of entries currently in the cache (expired or not).                   |

Every address is a `dmroute_addr_t` from
[dmroute](https://github.com/choco-technologies/dmroute); MAC addresses use
`dmnetif_mac_addr_t` from
[dmnetif](https://github.com/choco-technologies/dmnetif). Full
parameter/return documentation (including every error code) lives in
[docs/api-reference.md](docs/api-reference.md).

## Documentation

See the `docs/` directory:

- **[dmarp.md](docs/dmarp.md)** - Overview and architecture
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmarp`.

## Dependencies

- [`dmnetif`](https://github.com/choco-technologies/dmnetif) -
  `dmnetif_send()` to transmit a request, `dmnetif_get_mac_address()`/
  `_get_ip_address()` to build one, `dmnetif_get_name()` to key cache
  entries. `dmarp_resolve()` never calls `dmnetif_receive()` itself - see
  [Key design points](#key-design-points).
- [`dmroute`](https://github.com/choco-technologies/dmroute) - the shared
  `dmroute_addr_t` type every address field uses.
- `dmlist` - backs the ARP cache.
- `dmosi` - the mutex guarding the cache, `dmosi_get_tick_count()` for
  cache TTLs, and the semaphore `dmarp_resolve()` waits on and
  `dmarp_note_frame()` posts.

## Project Structure

```
dmarp/
├── docs/              # Documentation (markdown format)
│   ├── README.md
│   ├── dmarp.md
│   └── api-reference.md
├── include/           # Public headers
│   └── dmarp.h
├── src/
│   └── dmarp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmarp_test.c
├── CMakeLists.txt
├── Makefile
├── manifest.dmm
└── dmarp.dmr
```

## Author

Patryk Kubiak

## License

MIT License (see [LICENSE](LICENSE) file for details)
