/**
 * @file dmarp_test.c
 * @brief Test steps for dmarp
 *
 * Two fixture interfaces ("test0"/"test1") are registered against
 * "/dev/null" (real enough for Dmod_FileOpen() to succeed without a real
 * driver - same pattern as lib/dmnetif/tests/dmnetif_test.c). No real
 * driver means neither can ever go "up" or answer DMDRVI_IOCTL_NET_*
 * calls (they all hit dmod's weak default Dmod_Ioctl(), which returns
 * -ENOSYS) - dmarp_resolve() on a cache miss against either one always
 * fails fast with -ENODEV (dmnetif_get_mac_address() fails before a
 * request could even be sent), so the reply-wait/timeout path itself is
 * not exercised here. What *is* covered: the cache (insert/lookup/
 * remove/count) directly, and dmarp_resolve()'s cache-hit path, which is
 * exactly the same cache underneath.
 *
 * Every step uses a distinct IP address (never reused across steps) since
 * the cache is keyed by interface *name* - a value that survives a
 * fixture being unregistered and re-registered by consecutive
 * setup/teardown calls - so cache entries genuinely persist across test
 * steps for the same name unless each step's addresses are kept unique.
 */
#include "dmod_test.h"
#include "dmarp.h"
#include "dmnetif.h"
#include <string.h>
#include <errno.h>

#define TEST_DEVICE_PATH_0 "/null"
#define TEST_DEVICE_PATH_1 "/null2"

static dmnetif_iface_t g_iface0 = NULL;
static dmnetif_iface_t g_iface1 = NULL;

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t ip = { 0 };
    ip.family = dmroute_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

static dmnetif_mac_addr_t make_mac(uint8_t last_octet)
{
    dmnetif_mac_addr_t mac = { { 0x02, 0x00, 0x00, 0x00, 0x00, last_octet } };
    return mac;
}

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it. */
static bool mac_equal(const dmnetif_mac_addr_t* a, const dmnetif_mac_addr_t* b)
{
    for (int i = 0; i < DMNETIF_MAC_ADDR_LEN; i++)
    {
        if (a->addr[i] != b->addr[i])
            return false;
    }
    return true;
}

/* Byte offsets/values mirror src/dmarp.c's own documented frame layout
 * (Ethernet II + ARP, IPv4-over-Ethernet, 42 bytes) - kept independent of
 * the internal build_request_frame()/parse_sender_from_frame() helpers so
 * this test builds frames the way a peer on the wire actually would,
 * rather than reusing dmarp's own (possibly buggy) construction code. */
#define TEST_ARP_FRAME_LEN     42u
#define TEST_ARP_ETHERTYPE     0x0806u
#define TEST_ARP_HTYPE_ETH     1u
#define TEST_ARP_PTYPE_IPV4    0x0800u
#define TEST_ARP_OP_REQUEST    1u
#define TEST_ARP_OP_REPLY      2u

static void write_u16_be(uint8_t* p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFF);
}

static void build_arp_frame(uint8_t* frame, uint16_t opcode, const dmnetif_mac_addr_t* sender_mac, const dmroute_addr_t* sender_ip)
{
    memset(frame, 0, TEST_ARP_FRAME_LEN);

    memset(&frame[0], 0xFF, DMNETIF_MAC_ADDR_LEN); /* destination MAC - unused by dmarp_note_frame() */
    memcpy(&frame[6], sender_mac->addr, DMNETIF_MAC_ADDR_LEN);
    write_u16_be(&frame[12], TEST_ARP_ETHERTYPE);

    uint8_t* arp = &frame[14];
    write_u16_be(&arp[0], TEST_ARP_HTYPE_ETH);
    write_u16_be(&arp[2], TEST_ARP_PTYPE_IPV4);
    arp[4] = DMNETIF_MAC_ADDR_LEN;
    arp[5] = DMROUTE_IPV4_ADDR_LEN;
    write_u16_be(&arp[6], opcode);
    memcpy(&arp[8], sender_mac->addr, DMNETIF_MAC_ADDR_LEN);
    memcpy(&arp[14], sender_ip->addr.v4, DMROUTE_IPV4_ADDR_LEN);
    /* arp[18..27] (target hw/proto addr) left zeroed - not read by dmarp_note_frame() */
}

void dmod_test_setup(void)
{
    g_iface0 = dmnetif_register("test0", TEST_DEVICE_PATH_0);
    g_iface1 = dmnetif_register("test1", TEST_DEVICE_PATH_1);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface0);
    dmnetif_unregister(g_iface1);
    g_iface0 = NULL;
    g_iface1 = NULL;
}

/* ---- Cache: lookup / insert ---- */

DMOD_TEST_STEP(lookup_unknown_entry_returns_false)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 1);
    dmnetif_mac_addr_t mac = { 0 };
    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, &ip, &mac));
}

DMOD_TEST_STEP(insert_then_lookup_roundtrips)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 2);
    dmnetif_mac_addr_t mac = make_mac(0x02);
    dmarp_cache_insert(g_iface0, &ip, &mac);

    dmnetif_mac_addr_t got = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface0, &ip, &got));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got, &mac));

    dmarp_cache_remove(g_iface0, &ip);
}

DMOD_TEST_STEP(insert_replaces_existing_entry_without_growing_cache)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 3);
    dmnetif_mac_addr_t first_mac = make_mac(0x03);
    dmnetif_mac_addr_t second_mac = make_mac(0x04);

    dmarp_cache_insert(g_iface0, &ip, &first_mac);
    size_t count_after_first = dmarp_cache_count();

    dmarp_cache_insert(g_iface0, &ip, &second_mac);
    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), count_after_first);

    dmnetif_mac_addr_t got = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface0, &ip, &got));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got, &second_mac));

    dmarp_cache_remove(g_iface0, &ip);
}

DMOD_TEST_STEP(same_ip_on_different_interfaces_are_independent_entries)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 4);
    dmnetif_mac_addr_t mac0 = make_mac(0x05);
    dmnetif_mac_addr_t mac1 = make_mac(0x06);

    dmarp_cache_insert(g_iface0, &ip, &mac0);
    dmarp_cache_insert(g_iface1, &ip, &mac1);

    dmnetif_mac_addr_t got0 = { 0 };
    dmnetif_mac_addr_t got1 = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface0, &ip, &got0));
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface1, &ip, &got1));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got0, &mac0));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got1, &mac1));

    dmarp_cache_remove(g_iface0, &ip);
    dmarp_cache_remove(g_iface1, &ip);
}

DMOD_TEST_STEP(insert_invalid_arguments_is_noop)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 5);
    dmnetif_mac_addr_t mac = make_mac(0x07);
    size_t before = dmarp_cache_count();

    dmarp_cache_insert(NULL, &ip, &mac);
    dmarp_cache_insert(g_iface0, NULL, &mac);
    dmarp_cache_insert(g_iface0, &ip, NULL);

    dmroute_addr_t bad_family_ip = ip;
    bad_family_ip.family = dmroute_family_v6;
    dmarp_cache_insert(g_iface0, &bad_family_ip, &mac);

    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before);
}

DMOD_TEST_STEP(lookup_invalid_arguments_return_false)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 6);
    dmnetif_mac_addr_t mac = { 0 };

    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(NULL, &ip, &mac));
    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, NULL, &mac));
    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, &ip, NULL));

    dmroute_addr_t bad_family_ip = ip;
    bad_family_ip.family = dmroute_family_none;
    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, &bad_family_ip, &mac));
}

/* ---- Cache: remove / count ---- */

DMOD_TEST_STEP(remove_removes_entry)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 7);
    dmnetif_mac_addr_t mac = make_mac(0x08);
    dmarp_cache_insert(g_iface0, &ip, &mac);

    size_t before = dmarp_cache_count();
    dmarp_cache_remove(g_iface0, &ip);
    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before - 1);

    dmnetif_mac_addr_t got = { 0 };
    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, &ip, &got));
}

DMOD_TEST_STEP(remove_unknown_entry_is_safe)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 8);
    size_t before = dmarp_cache_count();

    dmarp_cache_remove(g_iface0, &ip);
    dmarp_cache_remove(NULL, &ip);

    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before);
}

/* ---- Resolve ---- */

DMOD_TEST_STEP(resolve_returns_cached_entry_without_sending)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 9);
    dmnetif_mac_addr_t cached_mac = make_mac(0x09);
    dmarp_cache_insert(g_iface0, &ip, &cached_mac);

    /* g_iface0 can never actually send (no real driver, never brought
     * up) - a cache hit is the only way this can return 0. */
    dmnetif_mac_addr_t resolved = { 0 };
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, &ip, &resolved, DMARP_DEFAULT_TIMEOUT_MS), 0);
    DMOD_TEST_EXPECT_TRUE(mac_equal(&resolved, &cached_mac));

    dmarp_cache_remove(g_iface0, &ip);
}

DMOD_TEST_STEP(resolve_cache_miss_without_real_driver_returns_enodev)
{
    /* No real driver behind the fixture means dmnetif_get_mac_address()
     * itself fails (DMDRVI_IOCTL_NET_GET_MAC_ADDR hits dmod's weak
     * default Dmod_Ioctl(), -ENOSYS) before a request could even be
     * built, so this fails fast rather than waiting out the timeout. */
    dmroute_addr_t ip = make_v4(10, 0, 0, 10);
    dmnetif_mac_addr_t resolved = { 0 };
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, &ip, &resolved, DMARP_DEFAULT_TIMEOUT_MS), -ENODEV);
}

DMOD_TEST_STEP(resolve_null_arguments_return_einval)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 11);
    dmnetif_mac_addr_t mac = { 0 };

    DMOD_TEST_EXPECT_EQ(dmarp_resolve(NULL, &ip, &mac, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, NULL, &mac, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, &ip, NULL, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

DMOD_TEST_STEP(resolve_non_v4_family_returns_einval)
{
    dmroute_addr_t ip = make_v4(10, 0, 0, 12);
    ip.family = dmroute_family_v6;
    dmnetif_mac_addr_t mac = { 0 };
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, &ip, &mac, DMARP_DEFAULT_TIMEOUT_MS), -EINVAL);
}

/* ---- note_frame: opportunistic learning ----
 *
 * dmarp_resolve() itself no longer reads frames off the wire (see dmarp.h's
 * top comment) - dmnetbridge's RX pump feeds every received frame to
 * dmarp_note_frame() instead, which is what these steps exercise directly,
 * standing in for that pump.
 */

DMOD_TEST_STEP(note_frame_caches_sender_from_request)
{
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 1);
    dmnetif_mac_addr_t sender_mac = make_mac(0x21);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REQUEST, &sender_mac, &sender_ip);

    dmarp_note_frame(g_iface0, frame, sizeof(frame));

    dmnetif_mac_addr_t got = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface0, &sender_ip, &got));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got, &sender_mac));

    dmarp_cache_remove(g_iface0, &sender_ip);
}

DMOD_TEST_STEP(note_frame_caches_sender_from_reply)
{
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 2);
    dmnetif_mac_addr_t sender_mac = make_mac(0x22);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REPLY, &sender_mac, &sender_ip);

    dmarp_note_frame(g_iface0, frame, sizeof(frame));

    dmnetif_mac_addr_t got = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmarp_cache_lookup(g_iface0, &sender_ip, &got));
    DMOD_TEST_EXPECT_TRUE(mac_equal(&got, &sender_mac));

    dmarp_cache_remove(g_iface0, &sender_ip);
}

DMOD_TEST_STEP(note_frame_unblocks_pending_resolve)
{
    /* dmarp_resolve() itself can never succeed against these no-driver
     * fixtures (see resolve_cache_miss_without_real_driver_returns_enodev),
     * so this checks the piece note_frame_unblocks_pending_resolve is
     * actually responsible for in isolation: a cache entry it inserts is
     * immediately visible to a concurrent cache_lookup(), which is exactly
     * what wakes dmarp_resolve()'s wait loop into finding it. */
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 3);
    dmnetif_mac_addr_t sender_mac = make_mac(0x23);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REPLY, &sender_mac, &sender_ip);

    DMOD_TEST_EXPECT_FALSE(dmarp_cache_lookup(g_iface0, &sender_ip, &sender_mac));

    dmarp_note_frame(g_iface0, frame, sizeof(frame));

    dmnetif_mac_addr_t resolved = { 0 };
    DMOD_TEST_EXPECT_EQ(dmarp_resolve(g_iface0, &sender_ip, &resolved, DMARP_DEFAULT_TIMEOUT_MS), 0);
    DMOD_TEST_EXPECT_TRUE(mac_equal(&resolved, &sender_mac));

    dmarp_cache_remove(g_iface0, &sender_ip);
}

DMOD_TEST_STEP(note_frame_ignores_non_arp_ethertype)
{
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 4);
    dmnetif_mac_addr_t sender_mac = make_mac(0x24);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REPLY, &sender_mac, &sender_ip);
    write_u16_be(&frame[12], 0x0800u); /* IPv4, not ARP */

    size_t before = dmarp_cache_count();
    dmarp_note_frame(g_iface0, frame, sizeof(frame));
    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before);
}

DMOD_TEST_STEP(note_frame_ignores_too_short_frame)
{
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 5);
    dmnetif_mac_addr_t sender_mac = make_mac(0x25);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REPLY, &sender_mac, &sender_ip);

    size_t before = dmarp_cache_count();
    dmarp_note_frame(g_iface0, frame, TEST_ARP_FRAME_LEN - 1);
    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before);
}

DMOD_TEST_STEP(note_frame_null_arguments_is_noop)
{
    dmroute_addr_t sender_ip = make_v4(10, 0, 1, 6);
    dmnetif_mac_addr_t sender_mac = make_mac(0x26);
    uint8_t frame[TEST_ARP_FRAME_LEN];
    build_arp_frame(frame, TEST_ARP_OP_REPLY, &sender_mac, &sender_ip);

    size_t before = dmarp_cache_count();
    dmarp_note_frame(NULL, frame, sizeof(frame));
    dmarp_note_frame(g_iface0, NULL, sizeof(frame));
    DMOD_TEST_EXPECT_EQ(dmarp_cache_count(), before);
}
