/**
 * @file dmarp.c
 * @brief DMOD ARP Resolver - Implementation
 *
 * Cache of resolved (interface, IP) -> MAC mappings backed by dmlist,
 * guarded by a single mutex - same registry shape as dmnetif.c's own
 * g_ifaces and dmroute.c's own g_routes. See dmarp.h and docs/dmarp.md
 * for the full rationale.
 *
 * ARP request/reply frames are built/parsed as raw byte buffers rather
 * than packed C structs - dmod's minimal module runtime gives no
 * guarantee about struct packing, and this is the same "index into a
 * uint8_t buffer by hand" approach tools/ip/src/ip.c and
 * tools/ifconfig/src/ifconfig.c already use for address parsing.
 *
 * Frame layout (Ethernet II + ARP, IPv4-over-Ethernet, 42 bytes total):
 *
 *   offset  0: destination MAC (6 bytes) - broadcast for a request
 *   offset  6: source MAC      (6 bytes) - the resolving interface's own MAC
 *   offset 12: ethertype       (2 bytes) - DMARP_ETHERTYPE_ARP
 *   offset 14: hardware type   (2 bytes) - DMARP_HTYPE_ETHERNET
 *   offset 16: protocol type   (2 bytes) - DMARP_PTYPE_IPV4
 *   offset 18: hw addr length  (1 byte)  - DMNETIF_MAC_ADDR_LEN
 *   offset 19: proto addr len  (1 byte)  - DMROUTE_IPV4_ADDR_LEN
 *   offset 20: opcode          (2 bytes) - DMARP_OP_REQUEST/_REPLY
 *   offset 22: sender hw addr  (6 bytes)
 *   offset 28: sender proto addr (4 bytes)
 *   offset 32: target hw addr  (6 bytes) - zero in a request (unknown)
 *   offset 38: target proto addr (4 bytes)
 */
#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmarp.h"
#include "dmlist.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

#define DMARP_ETH_HEADER_LEN    14u
#define DMARP_ARP_PAYLOAD_LEN   28u
#define DMARP_FRAME_LEN         (DMARP_ETH_HEADER_LEN + DMARP_ARP_PAYLOAD_LEN)

#define DMARP_ETHERTYPE_ARP     0x0806u
#define DMARP_HTYPE_ETHERNET    1u
#define DMARP_PTYPE_IPV4        0x0800u
#define DMARP_OP_REQUEST        1u
#define DMARP_OP_REPLY          2u

/**
 * @brief One cached (interface, IP) -> MAC mapping
 *
 * Never handed out to callers as a pointer/handle (unlike dmnetif_iface_t/
 * dmroute_route_t) - dmarp's public API only ever copies field values in
 * and out - so this needs no magic-guard field.
 */
struct dmarp_entry
{
    char                iface_name[DMNETIF_NAME_MAX_LEN + 1]; /**< Name of the interface this entry was cached against - a name, not a dmnetif_iface_t, so a stale entry across interface churn is merely wrong, never a dangling handle (same reasoning as dmroute's routes, see dmroute.h) */
    dmroute_addr_t      ip;                                   /**< Resolved address */
    dmnetif_mac_addr_t  mac;                                  /**< Resolved MAC address */
    uint32_t            cached_at;                            /**< dmosi_get_tick_count() value when this entry was last (re)written */
};

/**
 * @brief Cache of every currently known mapping (struct dmarp_entry*)
 *
 * Created in dmod_init(), destroyed in dmod_deinit(). Guarded by g_mutex.
 */
static dmlist_context_t* g_cache = NULL;

/**
 * @brief Mutex guarding g_cache against concurrent lookup/insert/remove calls
 */
static dmosi_mutex_t g_mutex = NULL;

/**
 * @brief Posted by dmarp_note_frame() whenever it caches a sender address
 *        from an observed ARP frame, waking any dmarp_resolve() call
 *        currently waiting for a reply
 *
 * A counting semaphore rather than an event: dmarp_resolve() re-checks the
 * cache itself on every wake, so an extra/unrelated post (a different
 * interface's or a different address's frame) just costs one harmless
 * extra cache lookup, never a missed wake. Created in dmod_init(),
 * destroyed in dmod_deinit().
 */
static dmosi_semaphore_t g_reply_signal = NULL;

/**
 * @brief Needle used by compare_cache_key() - a cache entry is identified
 *        by (interface name, IP), never by IP alone (the same address
 *        could in principle be reachable differently on two different
 *        interfaces)
 */
typedef struct
{
    const char*        iface_name;
    const dmroute_addr_t* ip;
} cache_key_t;

/**
 * @brief Compare four raw bytes for equality
 *
 * dmod's minimal module runtime has no libc memcmp() (see
 * dmod/src/module/string.c's replacement set) - a small manual comparison
 * stands in for it, same pattern used throughout this repo's tests and
 * tools/ip/src/ip.c.
 */
static bool ipv4_bytes_equal(const uint8_t* a, const uint8_t* b)
{
    for (int i = 0; i < DMROUTE_IPV4_ADDR_LEN; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

/**
 * @brief dmlist_compare_func_t matching a struct dmarp_entry against a
 *        cache_key_t needle (interface name + IP address, ignoring freshness)
 */
static int compare_cache_key(const void* data, const void* user_data)
{
    const struct dmarp_entry* entry = (const struct dmarp_entry*)data;
    const cache_key_t* key = (const cache_key_t*)user_data;

    if (strcmp(entry->iface_name, key->iface_name) != 0 || entry->ip.family != key->ip->family)
        return -1;

    return ipv4_bytes_equal(entry->ip.addr.v4, key->ip->addr.v4) ? 0 : -1;
}

/**
 * @brief dmlist_compare_func_t comparing two elements by pointer identity
 */
static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

/**
 * @brief Whether a cache entry is still within DMARP_CACHE_TTL_MS of its
 *        last (re)write
 *
 * Unsigned subtraction, so this stays correct across a dmosi_get_tick_count()
 * wraparound as long as the entry isn't older than ~49.7 days (UINT32_MAX ms) -
 * the same standard tick-comparison idiom used by dmarp_resolve()'s poll loop.
 */
static bool is_entry_fresh(const struct dmarp_entry* entry)
{
    return (uint32_t)(dmosi_get_tick_count() - entry->cached_at) < DMARP_CACHE_TTL_MS;
}

/**
 * @brief Shared lookup behind dmarp_cache_lookup() and dmarp_resolve()'s
 *        own cache check
 *
 * @return true if a fresh entry was found (and `mac` filled in)
 */
static bool cache_lookup(dmnetif_iface_t iface, const dmroute_addr_t* ip, dmnetif_mac_addr_t* mac)
{
    const char* iface_name = dmnetif_get_name(iface);
    if (iface_name == NULL)
        return false;

    cache_key_t key = { .iface_name = iface_name, .ip = ip };

    dmosi_mutex_lock(g_mutex);
    struct dmarp_entry* entry = (struct dmarp_entry*)dmlist_find(g_cache, &key, compare_cache_key);
    bool found = entry != NULL && is_entry_fresh(entry);
    if (found)
    {
        *mac = entry->mac;
    }
    dmosi_mutex_unlock(g_mutex);

    return found;
}

/**
 * @brief Shared insert behind dmarp_cache_insert() and dmarp_resolve()'s
 *        own cache update after a successful reply
 *
 * Replaces any existing entry for the same (iface, ip) rather than
 * appending a duplicate - both an expired entry getting refreshed and a
 * caller overwriting a manually-seeded one go through this same path.
 */
static void cache_insert(dmnetif_iface_t iface, const dmroute_addr_t* ip, const dmnetif_mac_addr_t* mac)
{
    const char* iface_name = dmnetif_get_name(iface);
    if (iface_name == NULL)
        return;

    cache_key_t key = { .iface_name = iface_name, .ip = ip };

    dmosi_mutex_lock(g_mutex);

    struct dmarp_entry* entry = (struct dmarp_entry*)dmlist_find(g_cache, &key, compare_cache_key);
    if (entry == NULL)
    {
        entry = Dmod_Malloc(sizeof(*entry));
        if (entry != NULL)
        {
            Dmod_SnPrintf(entry->iface_name, sizeof(entry->iface_name), "%s", iface_name);
            entry->ip = *ip;
            if (!dmlist_push_back(g_cache, entry))
            {
                Dmod_Free(entry);
                entry = NULL;
            }
        }
    }

    if (entry != NULL)
    {
        entry->mac = *mac;
        entry->cached_at = dmosi_get_tick_count();
    }

    dmosi_mutex_unlock(g_mutex);
}

/**
 * @brief Write a 16-bit value into a byte buffer in network (big-endian) order
 */
static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

/**
 * @brief Read a 16-bit network-order (big-endian) value from a byte buffer
 */
static uint16_t read_u16_be(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/**
 * @brief Build an ARP request frame - see this file's top comment for the
 *        exact byte layout
 *
 * @param frame     Output buffer, must be at least DMARP_FRAME_LEN bytes
 * @param local_mac The resolving interface's own MAC address
 * @param local_ip  The resolving interface's own IP address, or a
 *                  dmroute_family_none address if it doesn't have one yet
 *                  (written as all-zero sender protocol address, same as
 *                  a real host ARPing before it has an address of its
 *                  own - e.g. during DHCP)
 * @param target_ip The address being resolved (family must already be
 *                  dmroute_family_v4 - checked by dmarp_resolve() before
 *                  this is called)
 */
static void build_request_frame(uint8_t* frame, const dmnetif_mac_addr_t* local_mac, const dmroute_addr_t* local_ip, const dmroute_addr_t* target_ip)
{
    memset(frame, 0, DMARP_FRAME_LEN);

    memset(&frame[0], 0xFF, DMNETIF_MAC_ADDR_LEN); /* broadcast destination */
    memcpy(&frame[6], local_mac->addr, DMNETIF_MAC_ADDR_LEN);
    write_u16_be(&frame[12], DMARP_ETHERTYPE_ARP);

    uint8_t* arp = &frame[DMARP_ETH_HEADER_LEN];
    write_u16_be(&arp[0], DMARP_HTYPE_ETHERNET);
    write_u16_be(&arp[2], DMARP_PTYPE_IPV4);
    arp[4] = DMNETIF_MAC_ADDR_LEN;
    arp[5] = DMROUTE_IPV4_ADDR_LEN;
    write_u16_be(&arp[6], DMARP_OP_REQUEST);
    memcpy(&arp[8], local_mac->addr, DMNETIF_MAC_ADDR_LEN);
    if (local_ip->family == dmroute_family_v4)
    {
        memcpy(&arp[14], local_ip->addr.v4, DMROUTE_IPV4_ADDR_LEN);
    }
    /* arp[18..23] (target hw addr) already zeroed - unknown, that's the point */
    memcpy(&arp[24], target_ip->addr.v4, DMROUTE_IPV4_ADDR_LEN);
}

/**
 * @brief Check whether a received frame is a well-formed ARP request or
 *        reply and, if so, extract the sender's protocol+hardware address
 *
 * Deliberately opcode-agnostic and doesn't filter by sender/target address -
 * dmarp_note_frame() wants to learn from *any* ARP traffic it sees, not
 * just replies to a request we ourselves sent. See dmarp_note_frame()'s
 * doc comment for why a request is just as useful a source as a reply.
 *
 * @param frame     Received frame bytes
 * @param length    Number of valid bytes in `frame`
 * @param out_ip    Output buffer for the sender's protocol (IPv4) address
 * @param out_mac   Output buffer for the sender's MAC address
 *
 * @return true if `frame` is a well-formed ARP request or reply
 */
static bool parse_sender_from_frame(const uint8_t* frame, size_t length, dmroute_addr_t* out_ip, dmnetif_mac_addr_t* out_mac)
{
    if (length < DMARP_FRAME_LEN)
        return false;

    if (read_u16_be(&frame[12]) != DMARP_ETHERTYPE_ARP)
        return false;

    const uint8_t* arp = &frame[DMARP_ETH_HEADER_LEN];
    if (read_u16_be(&arp[0]) != DMARP_HTYPE_ETHERNET || read_u16_be(&arp[2]) != DMARP_PTYPE_IPV4)
        return false;

    if (arp[4] != DMNETIF_MAC_ADDR_LEN || arp[5] != DMROUTE_IPV4_ADDR_LEN)
        return false;

    uint16_t opcode = read_u16_be(&arp[6]);
    if (opcode != DMARP_OP_REQUEST && opcode != DMARP_OP_REPLY)
        return false;

    out_ip->family = dmroute_family_v4;
    memcpy(out_ip->addr.v4, &arp[14], DMROUTE_IPV4_ADDR_LEN);
    memcpy(out_mac->addr, &arp[8], DMNETIF_MAC_ADDR_LEN);
    return true;
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the ARP cache and its
 *        guarding mutex
 *
 * @param Config Module configuration (unused)
 *
 * @return 0 on success, -1 if the cache or mutex could not be allocated
 */
int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;

    g_cache = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_mutex = dmosi_mutex_create(false);
    g_reply_signal = dmosi_semaphore_create(0, UINT32_MAX);
    if (g_cache == NULL || g_mutex == NULL || g_reply_signal == NULL)
    {
        DMOD_LOG_ERROR("Failed to allocate dmarp cache\n");
        return -1;
    }

    DMOD_LOG_INFO("DMARP resolver initialized\n");
    return 0;
}

/**
 * @brief Module deinitialization - frees every cached entry, then the
 *        cache and its mutex
 *
 * @return Always 0
 */
int dmod_deinit(void)
{
    size_t count = dmlist_size(g_cache);
    for (size_t i = 0; i < count; i++)
    {
        Dmod_Free(dmlist_pop_front(g_cache));
    }
    dmlist_destroy(g_cache);
    g_cache = NULL;

    dmosi_mutex_destroy(g_mutex);
    g_mutex = NULL;

    dmosi_semaphore_destroy(g_reply_signal);
    g_reply_signal = NULL;

    DMOD_LOG_INFO("DMARP resolver deinitialized\n");
    return 0;
}

/* ---- Resolution ---- */

/**
 * @brief Implementation of dmarp_resolve() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, int, _resolve, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, dmnetif_mac_addr_t* mac, uint32_t timeout_ms ))
{
    if (iface == NULL || ip == NULL || mac == NULL)
        return -EINVAL;

    if (ip->family != dmroute_family_v4)
        return -EINVAL;

    if (cache_lookup(iface, ip, mac))
        return 0;

    dmnetif_mac_addr_t local_mac = { 0 };
    if (dmnetif_get_mac_address(iface, &local_mac) != 0)
    {
        DMOD_LOG_ERROR("dmarp_resolve: invalid interface\n");
        return -ENODEV;
    }

    dmroute_addr_t local_ip = { 0 };
    dmnetif_get_ip_address(iface, &local_ip); /* best-effort - family_none is fine, see build_request_frame() */

    uint8_t request[DMARP_FRAME_LEN];
    build_request_frame(request, &local_mac, &local_ip, ip);

    if (dmnetif_send(iface, request, sizeof(request)) == 0)
    {
        DMOD_LOG_ERROR("dmarp_resolve: failed to send request (interface down?)\n");
        return -EIO;
    }

    /* Can't poll dmnetif_receive() here anymore - once networkd starts,
     * dmnetbridge's per-interface RX pump is the only code path allowed to
     * read this interface (see dmnetbridge_handle_netif_rx()'s doc
     * comment). Instead, wait for dmarp_note_frame() to observe a matching
     * reply (fed to it by that same pump) and cache it, re-checking the
     * cache ourselves on every wake. */
    uint32_t deadline = dmosi_get_tick_count() + timeout_ms;
    for (;;)
    {
        int32_t remaining = (int32_t)(deadline - dmosi_get_tick_count());
        if (remaining <= 0)
            break;

        dmosi_semaphore_wait(g_reply_signal, 1, remaining);

        if (cache_lookup(iface, ip, mac))
            return 0;
    }

    return -ETIMEDOUT;
}

/**
 * @brief Implementation of dmarp_note_frame() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, void, _note_frame, ( dmnetif_iface_t iface, const uint8_t* frame, size_t frame_len ))
{
    if (iface == NULL || frame == NULL)
        return;

    dmroute_addr_t sender_ip;
    dmnetif_mac_addr_t sender_mac;
    if (!parse_sender_from_frame(frame, frame_len, &sender_ip, &sender_mac))
        return;

    cache_insert(iface, &sender_ip, &sender_mac);
    dmosi_semaphore_post(g_reply_signal, 1);
}

/* ---- Cache ---- */

/**
 * @brief Implementation of dmarp_cache_lookup() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, bool, _cache_lookup, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, dmnetif_mac_addr_t* mac ))
{
    if (iface == NULL || ip == NULL || mac == NULL || ip->family != dmroute_family_v4)
        return false;

    return cache_lookup(iface, ip, mac);
}

/**
 * @brief Implementation of dmarp_cache_insert() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, void, _cache_insert, ( dmnetif_iface_t iface, const dmroute_addr_t* ip, const dmnetif_mac_addr_t* mac ))
{
    if (iface == NULL || ip == NULL || mac == NULL || ip->family != dmroute_family_v4)
        return;

    cache_insert(iface, ip, mac);
}

/**
 * @brief Implementation of dmarp_cache_remove() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, void, _cache_remove, ( dmnetif_iface_t iface, const dmroute_addr_t* ip ))
{
    if (ip == NULL)
        return;

    const char* iface_name = dmnetif_get_name(iface);
    if (iface_name == NULL)
        return;

    cache_key_t key = { .iface_name = iface_name, .ip = ip };

    dmosi_mutex_lock(g_mutex);
    struct dmarp_entry* entry = (struct dmarp_entry*)dmlist_find(g_cache, &key, compare_cache_key);
    bool removed = entry != NULL && dmlist_remove(g_cache, entry, compare_pointer);
    dmosi_mutex_unlock(g_mutex);

    if (removed)
    {
        Dmod_Free(entry);
    }
}

/**
 * @brief Implementation of dmarp_cache_count() - see dmarp.h
 */
dmod_dmarp_api_declaration(1.0, size_t, _cache_count, ( void ))
{
    dmosi_mutex_lock(g_mutex);
    size_t count = dmlist_size(g_cache);
    dmosi_mutex_unlock(g_mutex);
    return count;
}
