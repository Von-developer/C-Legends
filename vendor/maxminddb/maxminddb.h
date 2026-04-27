/**
 * maxminddb.h  —  Vendored single-header shim for C-Legends
 *
 * This is a self-contained, minimal implementation of the MaxMind DB reader
 * API (MMDB_open / MMDB_lookup_string / MMDB_get_value / MMDB_close).
 * It supports the GeoLite2-City.mmdb binary format (spec version 2.0).
 *
 * Drop GeoLite2-City.mmdb next to your binary (or pass a full path).
 * If the file is absent every lookup returns MMDB_FILE_OPEN_ERROR and
 * GeoLocator falls back to "Unknown" — the rest of the app keeps running.
 *
 * For production use replace this shim with the official libmaxminddb:
 *   https://github.com/maxmind/libmaxminddb
 */

#ifndef MAXMINDDB_H
#define MAXMINDDB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return-code constants (mirrors official API) ─────────────────────── */
#define MMDB_SUCCESS              0
#define MMDB_FILE_OPEN_ERROR      1
#define MMDB_CORRUPT_SEARCH_TREE  2
#define MMDB_INVALID_METADATA     3
#define MMDB_IO_ERROR             4
#define MMDB_OUT_OF_MEMORY        5
#define MMDB_UNKNOWN_DATABASE     6
#define MMDB_INVALID_DATA         7
#define MMDB_LOOKUP_PATH_MISMATCH 8
#define MMDB_RECORD_NOT_FOUND     9

/* ── Data-type tags ───────────────────────────────────────────────────── */
#define MMDB_DATA_TYPE_UTF8_STRING  2
#define MMDB_DATA_TYPE_DOUBLE       3
#define MMDB_DATA_TYPE_UINT16       5
#define MMDB_DATA_TYPE_UINT32       6
#define MMDB_DATA_TYPE_MAP          7
#define MMDB_DATA_TYPE_FLOAT        15

/* ── Data entry (returned by MMDB_get_value) ──────────────────────────── */
typedef struct MMDB_entry_data_s {
    int             has_data;
    int             type;
    union {
        double      double_value;
        float       float_value;
        uint32_t    uint32;
        uint16_t    uint16;
        const char* utf8_string;
    };
    uint32_t data_size;   /* byte-length for utf8_string */
} MMDB_entry_data_s;

/* ── Entry handle (offset into the database) ──────────────────────────── */
typedef struct MMDB_entry_s {
    const struct MMDB_s* mmdb;
    uint32_t             offset;
} MMDB_entry_s;

/* ── Lookup result ────────────────────────────────────────────────────── */
typedef struct MMDB_lookup_result_s {
    int          found_entry;
    MMDB_entry_s entry;
    uint16_t     netmask;
} MMDB_lookup_result_s;

/* ── Metadata ─────────────────────────────────────────────────────────── */
typedef struct MMDB_metadata_s {
    uint32_t    node_count;
    uint16_t    record_size;   /* bits per record: 24, 28, or 32 */
    uint16_t    ip_version;    /* 4 or 6 */
    char*       database_type;
    uint32_t    binary_format_major_version;
    uint32_t    binary_format_minor_version;
} MMDB_metadata_s;

/* ── Main database handle ─────────────────────────────────────────────── */
typedef struct MMDB_s {
    uint32_t         flags;
    char*            filename;
    /* Raw file mapping */
    const uint8_t*   file_content;
    size_t           file_size;
    /* Parsed sections */
    const uint8_t*   data_section;
    size_t           data_section_size;
    uint32_t         data_section_offset_adjustment;
    MMDB_metadata_s  metadata;
    /* Private implementation pointer */
    void*            _impl;
} MMDB_s;

/* ── Public API ───────────────────────────────────────────────────────── */
int  MMDB_open(const char* filename, uint32_t flags, MMDB_s* mmdb);
void MMDB_close(MMDB_s* mmdb);

MMDB_lookup_result_s MMDB_lookup_string(const MMDB_s* mmdb,
                                         const char*   ipstr,
                                         int*          gai_error,
                                         int*          mmdb_error);

int MMDB_get_value(MMDB_entry_s* entry,
                   MMDB_entry_data_s* entry_data,
                   ...);

const char* MMDB_strerror(int error_code);

#define MMDB_MODE_MMAP 1
#define MMDB_MODE_MASK 7

#ifdef __cplusplus
}
#endif
#endif /* MAXMINDDB_H */
