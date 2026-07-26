#ifndef DMARP_H
#define DMARP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmroute.h"
#include "dmnetif.h"
#include "dmarp_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmarp.h
 * @brief DMOD ARP Resolver - Public API
 *
 * dmarp resolves an IPv4 address to a MAC address on a directly-connected
 * link, using ARP (RFC 826) request/reply frames sent and received
 * through dmnetif_send()/dmnetif_receive() on a specific interface.
 * Successful resolutions are cached (keyed by interface + IP address) so
 * repeated lookups of the same destination don't send a fresh request
 * every time - see dmarp_resolve() and the cache functions below.
 *
 * IPv6 is out of scope: IPv6 neighbor discovery uses NDP (over ICMPv6),
 * not ARP, so every function here only ever deals with
 * dmroute_family_v4 addresses.
 *
 * dmarp depends on dmnetif (to send frames and read an interface's own
 * MAC/IP address) and dmroute (for dmroute_addr_t) - dmnetbridge depends
 * on dmarp in turn (to resolve a destination MAC before sending, and to
 * feed it every received frame - see dmarp_note_frame()), not the other
 * way around, so this stays a plain one-way dependency chain, not a
 * DIF/MAL. There is exactly one ARP cache per system - functions here are
 * plain Built-in API (dmod_dmarp_api).
 *
 * dmarp does not read frames off the wire itself: since dmnetbridge's
 * per-interface RX pump (see its own docs) is the only code path allowed
 * to call dmnetif_receive() on a given interface once networkd owns it,
 * dmarp_resolve() cannot poll for a reply on its own anymore - it sends
 * its request, then waits on a signal that dmarp_note_frame() raises
 * whenever *any* ARP frame comes in through dmnetbridge, re-checking the
 * cache each time it wakes. See dmarp_note_frame()'s own doc comment.
 */

/**
 * @brief How long a cache entry stays valid before dmarp_resolve() treats
 *        it as a miss and sends a fresh request
 *
 * Applies uniformly to every entry, however it got there (a real ARP
 * reply via dmarp_resolve(), or a manual dmarp_cache_insert()) - dmarp
 * has no separate "static/permanent" entry concept. 60 seconds is a
 * conservative default inspired by real ARP cache timeouts (which range
 * from tens of seconds to tens of minutes depending on the stack) - short
 * enough that a NIC swap or DHCP re-lease on the far end doesn't stay
 * hidden for long.
 */
#define DMARP_CACHE_TTL_MS (60u * 1000u)

/**
 * @brief Default timeout for dmarp_resolve() to wait for a reply on a
 *        cache miss
 */
#define DMARP_DEFAULT_TIMEOUT_MS 1000u

/* ============================================================================
 *                      Resolution
 * ========================================================================== */

/**
 * @brief Resolve an IPv4 address to a MAC address on iface
 *
 * Checks the cache first; on a hit, `mac` is filled in immediately and no
 * frame is sent (`timeout_ms` is ignored). On a miss, sends one ARP
 * request out `iface` and waits for dmarp_note_frame() to observe a
 * matching reply and cache it (see that function - it's fed every
 * received frame by dmnetbridge's per-interface RX pump, the only code
 * path allowed to actually read from `iface`), re-checking the cache each
 * time it's woken, until either a match appears or `timeout_ms` elapses.
 *
 * Sends exactly one request - no retries. A caller that wants retry
 * behavior can just call dmarp_resolve() again after a timeout.
 *
 * @param iface      Interface to resolve through. Must be up with a real
 *                    driver behind it for a cache miss to ever succeed -
 *                    dmnetif_send() silently fails (see its own docs) on
 *                    a down interface, which dmarp_resolve() reports as
 *                    -EIO rather than waiting out the timeout for nothing.
 * @param ip         Address to resolve (family must be dmroute_family_v4)
 * @param mac        Output buffer for the resolved MAC address
 * @param timeout_ms Milliseconds to wait for a reply on a cache miss (use
 *                    DMARP_DEFAULT_TIMEOUT_MS if you don't have a strong
 *                    opinion)
 *
 * @return 0 on success, negative errno on failure:
 *         -EINVAL    NULL argument or `ip->family` is not dmroute_family_v4
 *         -ENODEV    `iface` is not a valid, currently registered interface
 *         -EIO       the request could not be sent (interface down, or
 *                    the driver rejected the frame)
 *         -ETIMEDOUT no matching reply arrived within `timeout_ms`
 */
dmod_dmarp_api(1.0, int, _resolve, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, dmnetif_mac_addr_t* mac, uint32_t timeout_ms ));

/**
 * @brief Feed one received frame to dmarp for opportunistic ARP learning
 *
 * Called by dmnetbridge's per-interface RX pump (dmnetbridge_handle_netif_rx())
 * for *every* frame it reads off `iface`, not just ones dmarp_resolve() is
 * actively waiting on - a plain Built-in API call, not a DIF, since
 * dmnetbridge already hard-depends on dmarp for dmarp_resolve() itself.
 *
 * Frames that aren't a well-formed ARP request or reply are silently
 * ignored. Otherwise the sender's (interface, IP) -> MAC mapping is cached
 * (dmarp_cache_insert(), same TTL as any other entry) regardless of
 * opcode or who the frame was addressed to - both a request and a reply
 * carry a usable sender address, and caching from a request too (not just
 * replies to our own dmarp_resolve() calls) is what lets repeated sends to
 * a peer we've merely *heard from* skip a fresh ARP round trip. Any
 * pending dmarp_resolve() call on any interface is then woken to
 * re-check the cache.
 *
 * @param iface     Interface the frame was received on
 * @param frame     Received frame bytes (full Ethernet frame, as passed to
 *                   dmnetif_receive())
 * @param frame_len Number of valid bytes in `frame`
 */
dmod_dmarp_api(1.0, void, _note_frame, ( dmnetif_iface_t iface, const uint8_t* frame, size_t frame_len ));

/* ============================================================================
 *                      Cache
 * ========================================================================== */

/**
 * @brief Look up a cached MAC address without sending a request
 *
 * Same cache dmarp_resolve() itself checks first - this is for a caller
 * that wants to know "do we already know this?" without ever blocking or
 * sending a frame, or that wants "resolve, but only from cache" (a
 * dmarp_resolve() call with a fresh cache entry does exactly this
 * already, but this skips the argument re-validation and is more
 * explicit about intent).
 *
 * @param iface Interface the entry would have been cached against
 * @param ip    Address to look up (family must be dmroute_family_v4)
 * @param mac   Output buffer for the cached MAC address
 *
 * @return true if a non-expired cache entry was found (and `mac` filled
 *         in), false otherwise (including invalid arguments)
 */
dmod_dmarp_api(1.0, bool, _cache_lookup, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, dmnetif_mac_addr_t* mac ));

/**
 * @brief Add or replace a cache entry
 *
 * Called internally by dmarp_resolve() after a successful reply; exposed
 * publicly so a caller can seed a known mapping by hand (e.g. a
 * hardcoded gateway MAC, or a test fixture) without waiting on a real
 * ARP exchange. Subject to the same DMARP_CACHE_TTL_MS as any other
 * entry - see its doc comment.
 *
 * @param iface Interface to cache the entry against
 * @param ip    Address to cache (family must be dmroute_family_v4)
 * @param mac   MAC address to associate with `ip`
 */
dmod_dmarp_api(1.0, void, _cache_insert, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, const dmnetif_mac_addr_t* mac ));

/**
 * @brief Remove one cache entry, if present
 *
 * For invalidating a specific stale mapping by hand (e.g. after detecting
 * an address conflict) rather than waiting out DMARP_CACHE_TTL_MS. Safe
 * to call when no matching entry exists.
 *
 * @param iface Interface the entry was cached against
 * @param ip    Address to forget
 */
dmod_dmarp_api(1.0, void, _cache_remove, ( dmnetif_iface_t iface, const dmroute_addr_t* ip ));

/**
 * @brief Number of entries currently in the cache (expired or not)
 *
 * Entries are only actually pruned when looked up (dmarp_resolve()/
 * _cache_lookup() treat an expired entry as a miss) or explicitly removed
 * - this counts everything still sitting in the table either way.
 */
dmod_dmarp_api(1.0, size_t, _cache_count, ( void ));

#ifdef __cplusplus
}
#endif

#endif // DMARP_H
