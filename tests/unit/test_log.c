/*
 * test_log.c — tests for logging subsystem
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/log.h"
#include "test_harness.h"

/* test sink that captures last message */
static char last_msg[512];
static int  sink_call_count;

static void test_sink(odfs_log_level_t level, odfs_log_subsys_t subsys,
                      const char *msg, void *ctx)
{
    (void)level; (void)subsys; (void)ctx;
    strncpy(last_msg, msg, sizeof(last_msg) - 1);
    last_msg[sizeof(last_msg) - 1] = '\0';
    sink_call_count++;
}

static void reset_sink(void)
{
    last_msg[0] = '\0';
    sink_call_count = 0;
}

TEST(log_init_defaults)
{
    odfs_log_state_t state;
    odfs_log_init(&state);

    ASSERT(state.subsys_mask == ~0u);
    ASSERT(state.sink.write == NULL);
}

TEST(log_null_sink_no_crash)
{
    odfs_log_state_t state;
    odfs_log_init(&state);
    /* no sink set — should not crash */
    odfs_log(&state, ODFS_LOG_ERROR, ODFS_SUB_CORE, "test %d", 42);
}

TEST(log_basic_output)
{
    odfs_log_state_t state;
    odfs_log_init(&state);
    odfs_log_set_sink(&state, test_sink, NULL);
    odfs_log_set_level(&state, ODFS_LOG_TRACE);
    reset_sink();

    odfs_log(&state, ODFS_LOG_ERROR, ODFS_SUB_CORE, "hello %d", 42);
    ASSERT_EQ(sink_call_count, 1);
    ASSERT(strstr(last_msg, "ERROR") != NULL);
    ASSERT(strstr(last_msg, "core") != NULL);
    ASSERT(strstr(last_msg, "hello 42") != NULL);
}

TEST(log_level_filtering)
{
    odfs_log_state_t state;
    odfs_log_init(&state);
    odfs_log_set_sink(&state, test_sink, NULL);
    odfs_log_set_level(&state, ODFS_LOG_WARN);
    reset_sink();

    odfs_log(&state, ODFS_LOG_DEBUG, ODFS_SUB_CORE, "filtered");
    ASSERT_EQ(sink_call_count, 0);

    odfs_log(&state, ODFS_LOG_WARN, ODFS_SUB_CORE, "visible");
    ASSERT_EQ(sink_call_count, 1);
}

TEST(log_subsys_mask_filtering)
{
    odfs_log_state_t state;
    odfs_log_init(&state);
    odfs_log_set_sink(&state, test_sink, NULL);
    odfs_log_set_level(&state, ODFS_LOG_TRACE);
    /* only enable CORE subsystem */
    odfs_log_set_subsys_mask(&state, 1u << ODFS_SUB_CORE);
    reset_sink();

    odfs_log(&state, ODFS_LOG_ERROR, ODFS_SUB_ISO, "filtered");
    ASSERT_EQ(sink_call_count, 0);

    odfs_log(&state, ODFS_LOG_ERROR, ODFS_SUB_CORE, "visible");
    ASSERT_EQ(sink_call_count, 1);
}

TEST(log_enabled_check)
{
    odfs_log_state_t state;
    odfs_log_init(&state);
    odfs_log_set_sink(&state, test_sink, NULL);
    odfs_log_set_level(&state, ODFS_LOG_WARN);

    ASSERT(odfs_log_enabled(&state, ODFS_LOG_ERROR, ODFS_SUB_CORE));
    ASSERT(odfs_log_enabled(&state, ODFS_LOG_WARN, ODFS_SUB_CORE));
    ASSERT(!odfs_log_enabled(&state, ODFS_LOG_INFO, ODFS_SUB_CORE));
    ASSERT(!odfs_log_enabled(&state, ODFS_LOG_DEBUG, ODFS_SUB_CORE));
}

TEST(log_level_names)
{
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_FATAL), "FATAL");
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_ERROR), "ERROR");
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_WARN),  "WARN");
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_INFO),  "INFO");
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_DEBUG), "DEBUG");
    ASSERT_STR_EQ(odfs_log_level_name(ODFS_LOG_TRACE), "TRACE");
}

TEST(log_subsys_names)
{
    ASSERT_STR_EQ(odfs_log_subsys_name(ODFS_SUB_CORE), "core");
    ASSERT_STR_EQ(odfs_log_subsys_name(ODFS_SUB_ISO),  "iso");
    ASSERT_STR_EQ(odfs_log_subsys_name(ODFS_SUB_CACHE), "cache");
}

TEST_MAIN()
