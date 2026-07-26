# dmarp API Reference

See [dmarp.md](dmarp.md) for the architecture and rationale behind this
API's shape.

## Constants

| Constant                     | Value   | Description                                             |
|-------------------------------|---------|-------------------------------------------------------------|
| `DMARP_CACHE_TTL_MS`          | 60000   | How long a cache entry stays valid, however it got there. |
| `DMARP_DEFAULT_TIMEOUT_MS`    | 1000    | Suggested `timeout_ms` for `dmarp_resolve()` when you don't have a strong opinion. |

All addresses use `dmroute_addr_t` from dmroute; MAC
addresses use `dmnetif_mac_addr_t` from [dmnetif](../../dmnetif).

## Resolution

| Function                                                    | Description                                                        |
|------------------------------------------------------------------|-----------------------------------------------------------------------|
| `dmarp_resolve(iface, ip, mac, timeout_ms)`                      | Resolve `ip` to a MAC address on `iface`. Cache hit: immediate, `timeout_ms` ignored. Cache miss: sends one ARP request, then waits (does not poll `dmnetif_receive()` itself - see [dmarp.md](dmarp.md#resolution)) up to `timeout_ms` for `dmarp_note_frame()` to observe and cache a matching reply. Returns `0` on success, `-EINVAL` (bad argument/family), `-ENODEV` (invalid `iface`), `-EIO` (request could not be sent), or `-ETIMEDOUT`. |
| `dmarp_note_frame(iface, frame, frame_len)`                      | Feed one received frame (from `dmnetbridge_handle_netif_rx()`) to dmarp for opportunistic learning - caches the sender's mapping from any well-formed ARP request or reply, and wakes any pending `dmarp_resolve()` call. See [dmarp.md](dmarp.md#opportunistic-learning). |

## Cache

| Function                                              | Description                                                        |
|------------------------------------------------------------|-----------------------------------------------------------------------|
| `dmarp_cache_lookup(iface, ip, mac)`                       | Look up a cached entry without sending a request. Returns `true` (and fills `mac`) on a fresh hit, `false` otherwise. |
| `dmarp_cache_insert(iface, ip, mac)`                       | Add or replace a cache entry - what `dmarp_resolve()` calls internally after a successful reply, also usable directly to seed a known mapping by hand. |
| `dmarp_cache_remove(iface, ip)`                            | Remove one entry, if present. Safe when no matching entry exists. |
| `dmarp_cache_count()`                                      | Number of entries currently in the cache (expired or not - only pruned on lookup or explicit removal). |
