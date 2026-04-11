/*
 * log.c — structured logging subsystem
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/log.h"
#include "odfs/printf.h"
#include <stdio.h>
#include <string.h>

static const char *level_names[] = {
    [ODFS_LOG_FATAL] = "FATAL",
    [ODFS_LOG_ERROR] = "ERROR",
    [ODFS_LOG_WARN]  = "WARN",
    [ODFS_LOG_INFO]  = "INFO",
    [ODFS_LOG_DEBUG] = "DEBUG",
    [ODFS_LOG_TRACE] = "TRACE",
};

static const char *subsys_names[] = {
    [ODFS_SUB_NONE]         = "",
    [ODFS_SUB_CORE]         = "core",
    [ODFS_SUB_DOS]          = "dos",
    [ODFS_SUB_MOUNT]        = "mount",
    [ODFS_SUB_IO]           = "io",
    [ODFS_SUB_ISO]          = "iso",
    [ODFS_SUB_RR]           = "rr",
    [ODFS_SUB_JOLIET]       = "joliet",
    [ODFS_SUB_UDF]          = "udf",
    [ODFS_SUB_HFS]          = "hfs",
    [ODFS_SUB_HFSPLUS]      = "hfsplus",
    [ODFS_SUB_MULTISESSION] = "multisession",
    [ODFS_SUB_CDDA]         = "cdda",
    [ODFS_SUB_CACHE]        = "cache",
    [ODFS_SUB_CHARSET]      = "charset",
};

const char *odfs_log_level_name(odfs_log_level_t level)
{
    if (level >= 0 && level < ODFS_LOG__COUNT)
        return level_names[level];
    return "???";
}

const char *odfs_log_subsys_name(odfs_log_subsys_t subsys)
{
    if (subsys >= 0 && subsys < ODFS_SUB__COUNT)
        return subsys_names[subsys];
    return "???";
}

void odfs_log_init(odfs_log_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->max_level = ODFS_LOG_MAX_LEVEL;
    state->subsys_mask = ~0u; /* all subsystems enabled */
    state->sink.write = NULL;
    state->sink.ctx = NULL;
}

void odfs_log_set_sink(odfs_log_state_t *state, odfs_log_sink_fn fn, void *ctx)
{
    state->sink.write = fn;
    state->sink.ctx = ctx;
}

void odfs_log_set_level(odfs_log_state_t *state, odfs_log_level_t level)
{
    state->max_level = level;
}

void odfs_log_set_subsys_mask(odfs_log_state_t *state, unsigned int mask)
{
    state->subsys_mask = mask;
}

void odfs_log(odfs_log_state_t *state,
               odfs_log_level_t level,
               odfs_log_subsys_t subsys,
               const char *fmt, ...)
{
    char buf[512];
    int hdr;
    va_list ap;

    if (!odfs_log_enabled(state, level, subsys))
        return;

    if (subsys == ODFS_SUB_NONE) {
        hdr = odfs_snprintf(buf, sizeof(buf), "[%s] ",
                            odfs_log_level_name(level));
    } else {
        hdr = odfs_snprintf(buf, sizeof(buf), "[%s] %s: ",
                            odfs_log_level_name(level),
                            odfs_log_subsys_name(subsys));
    }
    if (hdr < 0)
        hdr = 0;

    va_start(ap, fmt);
    if ((size_t)hdr < sizeof(buf))
        odfs_vsnprintf(buf + hdr, sizeof(buf) - hdr, fmt, ap);
    va_end(ap);

    state->sink.write(level, subsys, buf, state->sink.ctx);
}
