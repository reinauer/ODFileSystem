/*
 * odfs/log.h — structured logging subsystem
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_LOG_H
#define ODFS_LOG_H

#include "odfs/config.h"
#include <stdarg.h>

/* --- log levels (ascending verbosity) --- */

typedef enum odfs_log_level {
    ODFS_LOG_FATAL = 0,
    ODFS_LOG_ERROR,
    ODFS_LOG_WARN,
    ODFS_LOG_INFO,
    ODFS_LOG_DEBUG,
    ODFS_LOG_TRACE,
    ODFS_LOG__COUNT
} odfs_log_level_t;

/* --- subsystem tags --- */

typedef enum odfs_log_subsys {
    ODFS_SUB_CORE = 0,
    ODFS_SUB_DOS,
    ODFS_SUB_MOUNT,
    ODFS_SUB_IO,
    ODFS_SUB_ISO,
    ODFS_SUB_RR,
    ODFS_SUB_JOLIET,
    ODFS_SUB_UDF,
    ODFS_SUB_HFS,
    ODFS_SUB_HFSPLUS,
    ODFS_SUB_MULTISESSION,
    ODFS_SUB_CDDA,
    ODFS_SUB_CACHE,
    ODFS_SUB_CHARSET,
    ODFS_SUB__COUNT
} odfs_log_subsys_t;

/* --- sink interface --- */

typedef void (*odfs_log_sink_fn)(odfs_log_level_t level,
                                  odfs_log_subsys_t subsys,
                                  const char *msg,
                                  void *ctx);

typedef struct odfs_log_sink {
    odfs_log_sink_fn write;
    void *ctx;
} odfs_log_sink_t;

/* --- runtime log state --- */

typedef struct odfs_log_state {
    odfs_log_level_t max_level;
    unsigned int      subsys_mask;   /* bitmask: 1 << ODFS_SUB_xxx */
    odfs_log_sink_t  sink;
} odfs_log_state_t;

/* --- API --- */

void odfs_log_init(odfs_log_state_t *state);
void odfs_log_set_sink(odfs_log_state_t *state, odfs_log_sink_fn fn, void *ctx);
void odfs_log_set_level(odfs_log_state_t *state, odfs_log_level_t level);
void odfs_log_set_subsys_mask(odfs_log_state_t *state, unsigned int mask);

void odfs_log(odfs_log_state_t *state,
               odfs_log_level_t level,
               odfs_log_subsys_t subsys,
               const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

void odfs_logv(odfs_log_state_t *state,
                odfs_log_level_t level,
                odfs_log_subsys_t subsys,
                const char *fmt,
                va_list ap);

/* convenience: check if a message would be emitted */
static inline int odfs_log_enabled(const odfs_log_state_t *state,
                                    odfs_log_level_t level,
                                    odfs_log_subsys_t subsys)
{
    if (!state || !state->sink.write) return 0;
    if (level > state->max_level) return 0;
    if (!(state->subsys_mask & (1u << subsys))) return 0;
    return 1;
}

/* helper macros */
#if ODFS_FEATURE_LOG
  #define ODFS_LOG(st, lv, sub, ...) \
      do { if (odfs_log_enabled((st), (lv), (sub))) \
               odfs_log((st), (lv), (sub), __VA_ARGS__); } while (0)
#else
  #define ODFS_LOG(st, lv, sub, ...) \
      do { (void)(st); (void)(lv); (void)(sub); } while (0)
#endif

/* shorthand per-level macros */
#define ODFS_FATAL(st, sub, ...) ODFS_LOG((st), ODFS_LOG_FATAL, (sub), __VA_ARGS__)
#define ODFS_ERROR(st, sub, ...) ODFS_LOG((st), ODFS_LOG_ERROR, (sub), __VA_ARGS__)
#define ODFS_WARN(st, sub, ...)  ODFS_LOG((st), ODFS_LOG_WARN,  (sub), __VA_ARGS__)
#define ODFS_INFO(st, sub, ...)  ODFS_LOG((st), ODFS_LOG_INFO,  (sub), __VA_ARGS__)
#define ODFS_DEBUG(st, sub, ...) ODFS_LOG((st), ODFS_LOG_DEBUG, (sub), __VA_ARGS__)
#define ODFS_TRACE(st, sub, ...) ODFS_LOG((st), ODFS_LOG_TRACE, (sub), __VA_ARGS__)

/* level / subsys name accessors */
const char *odfs_log_level_name(odfs_log_level_t level);
const char *odfs_log_subsys_name(odfs_log_subsys_t subsys);

#endif /* ODFS_LOG_H */
