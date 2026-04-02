/*
 * odfs/config.h — compile-time feature flags and build profiles
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_CONFIG_H
#define ODFS_CONFIG_H

/*
 * Build profiles:
 *   ODFS_PROFILE_FULL  — all features enabled (default)
 *   ODFS_PROFILE_ROM   — minimal ROM-capable subset
 *
 * Set exactly one profile, or configure individual flags below.
 */

#if !defined(ODFS_PROFILE_FULL) && !defined(ODFS_PROFILE_ROM)
#define ODFS_PROFILE_FULL
#endif

/* ---------- backend feature flags ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_ISO9660      1
  #define ODFS_FEATURE_ROCK_RIDGE   1
  #define ODFS_FEATURE_JOLIET       1
  #define ODFS_FEATURE_MULTISESSION 1
  #define ODFS_FEATURE_UDF          0
  #define ODFS_FEATURE_HFS          0
  #define ODFS_FEATURE_HFSPLUS      0
  #define ODFS_FEATURE_CDDA         0
#endif

#ifdef ODFS_PROFILE_FULL
  #define ODFS_FEATURE_ISO9660      1
  #define ODFS_FEATURE_ROCK_RIDGE   1
  #define ODFS_FEATURE_JOLIET       1
  #define ODFS_FEATURE_MULTISESSION 1
  #define ODFS_FEATURE_UDF          1
  #define ODFS_FEATURE_HFS          1
  #define ODFS_FEATURE_HFSPLUS      1
  #define ODFS_FEATURE_CDDA         1
#endif

/* ---------- cache feature flags ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_CACHE_BLOCK    1
  #define ODFS_FEATURE_CACHE_META     0
  #define ODFS_FEATURE_CACHE_STREAM   0
#else
  #define ODFS_FEATURE_CACHE_BLOCK    1
  #define ODFS_FEATURE_CACHE_META     1
  #define ODFS_FEATURE_CACHE_STREAM   1
#endif

/* ---------- logging feature flags ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_LOG            1
  #define ODFS_FEATURE_LOG_SERIAL     1
  #define ODFS_FEATURE_LOG_KPRINTF    0
  #define ODFS_FEATURE_LOG_RINGBUF    0
  #define ODFS_LOG_MAX_LEVEL          ODFS_LOG_WARN
#else
  #define ODFS_FEATURE_LOG            1
  #define ODFS_FEATURE_LOG_SERIAL     1
  #define ODFS_FEATURE_LOG_KPRINTF    1
  #define ODFS_FEATURE_LOG_RINGBUF    1
  #define ODFS_LOG_MAX_LEVEL          ODFS_LOG_TRACE
#endif

/* ---------- charset feature flags ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_CHARSET_BUILTIN   1
  #define ODFS_FEATURE_CHARSET_EXTERNAL  0
#else
  #define ODFS_FEATURE_CHARSET_BUILTIN   1
  #define ODFS_FEATURE_CHARSET_EXTERNAL  1
#endif

/* ---------- cache sizing defaults ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_BLOCK_CACHE_SIZE     16
  #define ODFS_META_CACHE_SIZE       0
#else
  #define ODFS_BLOCK_CACHE_SIZE    128
  #define ODFS_META_CACHE_SIZE      64
#endif

/* ---------- platform ---------- */

#if defined(AMIGA) || defined(__amigaos__)
  #define ODFS_PLATFORM_AMIGA 1
#else
  #define ODFS_PLATFORM_AMIGA 0
#endif

/* host builds for testing */
#ifndef ODFS_PLATFORM_HOST
  #if !ODFS_PLATFORM_AMIGA
    #define ODFS_PLATFORM_HOST 1
  #else
    #define ODFS_PLATFORM_HOST 0
  #endif
#endif

#endif /* ODFS_CONFIG_H */
