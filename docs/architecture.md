# Architecture Overview

## Layers

### A. AmigaDOS Handler Frontend
- Packet dispatch, mount lifecycle, locks, file handles
- Translation from internal node model to AmigaDOS structures
- Kept as small and policy-light as practical

### B. Core Common Layer
- Internal node model (`odfs_node_t`)
- Path resolution
- Cache manager coordination
- Error mapping, memory policy helpers
- Configuration/profile support

### C. Device / Media Access Layer
- Sector reads via `odfs_media_t` vtable
- Media geometry, session/TOC discovery
- Block size abstraction, retry policy
- Isolates hardware from parser logic

### D. Parser Backends
Pluggable readers implementing `odfs_backend_ops_t`:
- iso9660, rock_ridge, joliet
- udf, hfs, hfsplus
- cdda, multisession

### E. Auxiliary Services
- Logging (`odfs_log`)
- Charset conversion
- Cache statistics
- Debug tracing

## Internal Object Model

All backends produce `odfs_node_t` entries. The handler frontend never needs to know which backend produced a node.

## Format Precedence

### ISO-Family Hybrids

Priority, highest first:

1. Rock Ridge
2. Joliet
3. Plain ISO 9660

The ISO9660 backend is probed first because Rock Ridge detection happens during ISO-family mount evaluation. If Rock Ridge is present, it wins. If not, Joliet is preferred over plain ISO 9660 unless Joliet has been disabled.

### Bridge Discs (ISO + UDF)

- Default policy: prefer ISO-family content, including Rock Ridge or Joliet when available
- `prefer_udf`: prefer UDF instead
- `force_backend`: force a specific backend

### Hybrid HFS/ISO Discs

- Default policy: prefer ISO-family content on Amiga
- `prefer_hfs`: prefer HFS instead

### Mount Options That Affect Selection

- `force_backend`: force a specific backend
- `force_session`: force a specific session number
- `disable_rr`: disable Rock Ridge
- `disable_joliet`: disable Joliet
- `prefer_udf`: prefer UDF on bridge discs
- `prefer_hfs`: prefer HFS on hybrid discs
