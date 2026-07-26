#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmarp.h"

static dmarp_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmarp_create();
}

void dmod_test_teardown(void)
{
    dmarp_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmarp_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmarp_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmarp_is_valid(g_handle));
}

DMOD_TEST_STEP(dmarp_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmarp_destroy(NULL);
}
