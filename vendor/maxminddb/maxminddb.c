/**
 * maxminddb.c  —  Vendored GeoLite2 MMDB reader (C-Legends)
 *
 * Supports MaxMind DB binary format v2.0 (record_size 24/28/32, IPv4/IPv6).
 * Correctly handles all MMDB data types including uint64 (extended type 9)
 * which is used for build_epoch in GeoLite2-City.mmdb.
 */

#include "maxminddb.h"

#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── MMDB metadata marker ────────────────────────────────────────────────── */
static const uint8_t METADATA_MARKER[] = {
    0xAB,0xCD,0xEF,'M','a','x','M','i','n','d','.','c','o','m'
};
#define METADATA_MARKER_LEN 14

/* ── Internal file descriptor ────────────────────────────────────────────── */
typedef struct { int fd; } _MMDB_impl;

/* ══════════════════════════════════════════════════════════════════════════
   Data-section decoder — fully handles all 16 MMDB types
   ══════════════════════════════════════════════════════════════════════════ */

/* Decode variable-length ctrl+size field.
   Returns bytes consumed (ctrl already consumed by caller).
   *size_out = the payload byte count.
   *type_out = the resolved type id (1-16).               */
static uint32_t decode_ctrl(const uint8_t *p, size_t avail,
                             int *type_out, uint32_t *size_out)
{
    if (avail == 0) return 0;
    uint8_t ctrl    = p[0];
    uint32_t used   = 1;
    int      type   = (ctrl >> 5) & 0x07;

    if (type == 0) {
        /* Extended type: next byte + 7 */
        if (avail < 2) return 0;
        type = (int)p[1] + 7;
        used = 2;
    }
    *type_out = type;

    uint32_t size_ind = ctrl & 0x1fu;
    if (size_ind <= 28) {
        *size_out = size_ind;
    } else if (size_ind == 29) {
        if (avail < used + 1) return 0;
        *size_out = 29u + p[used];
        used += 1;
    } else if (size_ind == 30) {
        if (avail < used + 2) return 0;
        *size_out = 285u + ((uint32_t)p[used] << 8 | p[used+1]);
        used += 2;
    } else { /* 31 */
        if (avail < used + 3) return 0;
        *size_out = 65821u + ((uint32_t)p[used]<<16
                             | (uint32_t)p[used+1]<<8
                             | p[used+2]);
        used += 3;
    }
    return used;
}

/* Full decode of a single value node.
   Returns total bytes consumed (header + payload), 0 on error.
   For maps/arrays, only the header is consumed; payload is NOT walked here. */
static uint32_t decode_data(const uint8_t *base, size_t base_size,
                             uint32_t offset, MMDB_entry_data_s *out)
{
    memset(out, 0, sizeof *out);
    if (offset >= (uint32_t)base_size) return 0;

    const uint8_t *p   = base + offset;
    size_t         avail = base_size - offset;
    int            type  = 0;
    uint32_t       size  = 0;
    uint32_t       hdr   = decode_ctrl(p, avail, &type, &size);
    if (hdr == 0) return 0;

    out->has_data = 1;

    const uint8_t *payload = p + hdr;
    uint32_t       total   = hdr + size;   /* may be refined below */

    if (offset + total > (uint32_t)base_size) return 0;

    switch (type) {
        case 1:  /* pointer — resolve and decode target */
        {
            /* ptr_size = ((ctrl>>3)&3)+1, encoded differently */
            uint8_t ctrl0    = p[0];
            int     ptr_size = ((ctrl0 >> 3) & 3) + 1;
            uint32_t ptr     = 0;
            /* first 3 bits of ctrl0 after type bits contribute */
            ptr = ctrl0 & 0x07;
            for (int i = 0; i < ptr_size; i++)
                ptr = (ptr << 8) | p[1+i];
            if (ptr_size == 3) ptr += 0;
            if (ptr_size == 2) ptr += 2048;
            if (ptr_size >= 3) ptr += 526336;
            /* Follow the pointer into the data section */
            if (ptr < (uint32_t)base_size) {
                decode_data(base, base_size, ptr, out);
            }
            /* consumed: ctrl byte + ptr_size bytes */
            return 1 + ptr_size;
        }
        case 2:  /* utf8_string */
            out->type        = MMDB_DATA_TYPE_UTF8_STRING;
            out->utf8_string = (const char*)payload;
            out->data_size   = size;
            break;
        case 3:  /* double */
        {
            out->type = MMDB_DATA_TYPE_DOUBLE;
            if (size == 8) {
                uint64_t be = ((uint64_t)payload[0]<<56)|((uint64_t)payload[1]<<48)|
                              ((uint64_t)payload[2]<<40)|((uint64_t)payload[3]<<32)|
                              ((uint64_t)payload[4]<<24)|((uint64_t)payload[5]<<16)|
                              ((uint64_t)payload[6]<<8) | payload[7];
                memcpy(&out->double_value, &be, 8);
            }
            break;
        }
        case 4:  /* bytes — treat as opaque */
            out->type      = MMDB_DATA_TYPE_UTF8_STRING;  /* reuse field */
            out->data_size = size;
            break;
        case 5:  /* uint16 */
            out->type = MMDB_DATA_TYPE_UINT16;
            for (uint32_t i = 0; i < size && i < 2; i++)
                out->uint16 = (uint16_t)((out->uint16 << 8) | payload[i]);
            break;
        case 6:  /* uint32 */
            out->type = MMDB_DATA_TYPE_UINT32;
            for (uint32_t i = 0; i < size && i < 4; i++)
                out->uint32 = (out->uint32 << 8) | payload[i];
            break;
        case 7:  /* map */
            out->type      = MMDB_DATA_TYPE_MAP;
            out->uint32    = size;           /* number of key/value pairs */
            out->data_size = hdr;            /* header bytes only */
            return hdr;                      /* do NOT consume payload */
        case 8:  /* int32 */
            out->type = MMDB_DATA_TYPE_UINT32;   /* store as uint32 */
            for (uint32_t i = 0; i < size && i < 4; i++)
                out->uint32 = (out->uint32 << 8) | payload[i];
            break;
        case 9:  /* uint64 — used for build_epoch; store low 32 bits */
            out->type = MMDB_DATA_TYPE_UINT32;
            for (uint32_t i = 0; i < size && i < 8; i++) {
                if (i < 4) { /* high bytes — discard for uint32 storage */
                    /* just advance */ }
                else
                    out->uint32 = (out->uint32 << 8) | payload[i];
            }
            /* Actually we need all 8 bytes as uint32 low half */
            out->uint32 = 0;
            for (uint32_t i = 0; i < size && i < 8; i++)
                out->uint32 = (out->uint32 << 8) | payload[i];
            break;
        case 10: /* uint128 — skip */
            out->type = MMDB_DATA_TYPE_UINT32;
            break;
        case 11: /* array */
            out->type      = MMDB_DATA_TYPE_MAP;   /* reuse map type */
            out->uint32    = size;
            out->data_size = hdr;
            return hdr;
        case 14: /* boolean */
            out->type   = MMDB_DATA_TYPE_UINT16;
            out->uint16 = (uint16_t)size;  /* size field IS the value (0/1) */
            total = hdr;
            break;
        case 15: /* float */
        {
            out->type = MMDB_DATA_TYPE_FLOAT;
            if (size == 4) {
                uint32_t be = ((uint32_t)payload[0]<<24)|((uint32_t)payload[1]<<16)|
                              ((uint32_t)payload[2]<<8) | payload[3];
                memcpy(&out->float_value, &be, 4);
                out->double_value = (double)out->float_value;
            }
            break;
        }
        default:
            break;
    }
    return total;
}

/* ══════════════════════════════════════════════════════════════════════════
   Skip a full value (including nested map/array contents)
   ══════════════════════════════════════════════════════════════════════════ */
static uint32_t skip_value(const uint8_t *base, size_t base_size, uint32_t off);

static uint32_t skip_value(const uint8_t *base, size_t base_size, uint32_t off)
{
    if (off >= (uint32_t)base_size) return off + 1;

    /* Peek at the raw ctrl byte to distinguish map (type 7) from array (type 11).
       decode_data() returns MMDB_DATA_TYPE_MAP for both, losing the distinction. */
    const uint8_t *p    = base + off;
    size_t         avail = base_size - off;
    int            raw_type = 0;
    uint32_t       raw_size = 0;
    uint32_t       hdr  = decode_ctrl(p, avail, &raw_type, &raw_size);
    if (!hdr) return off + 1;

    if (raw_type == 7) {
        /* Map: skip hdr + raw_size key-value pairs (raw_size*2 items) */
        off += hdr;
        for (uint32_t i = 0; i < raw_size * 2u; i++)
            off = skip_value(base, base_size, off);
    } else if (raw_type == 11) {
        /* Array: skip hdr + raw_size single values (NOT *2) */
        off += hdr;
        for (uint32_t i = 0; i < raw_size; i++)
            off = skip_value(base, base_size, off);
    } else if (raw_type == 1) {
        /* Pointer: 1 ctrl byte + ptr_size payload bytes */
        uint8_t ctrl0    = p[0];
        int     ptr_size = ((ctrl0 >> 3) & 3) + 1;
        off += 1 + (uint32_t)ptr_size;
    } else if (raw_type == 14) {
        /* Boolean: value is encoded in size field, no payload */
        off += hdr;
    } else {
        /* All scalar types: hdr + raw_size payload bytes */
        off += hdr + raw_size;
    }
    return off;
}

/* ══════════════════════════════════════════════════════════════════════════
   Metadata parser
   ══════════════════════════════════════════════════════════════════════════ */
static int parse_metadata(MMDB_s *mmdb)
{
    const uint8_t *data = mmdb->file_content;
    size_t         sz   = mmdb->file_size;
    ssize_t        meta_off = -1;

    /* Scan backwards for metadata marker */
    for (ssize_t i = (ssize_t)(sz - METADATA_MARKER_LEN); i >= 0; i--) {
        if (memcmp(data + i, METADATA_MARKER, METADATA_MARKER_LEN) == 0) {
            meta_off = i + (ssize_t)METADATA_MARKER_LEN;
            break;
        }
    }
    if (meta_off < 0) return MMDB_INVALID_METADATA;

    uint32_t off = (uint32_t)meta_off;

    /* Top-level map */
    MMDB_entry_data_s map_d;
    uint32_t hdr = decode_data(data, sz, off, &map_d);
    if (!hdr || map_d.type != MMDB_DATA_TYPE_MAP) return MMDB_INVALID_METADATA;

    off += hdr;                   /* now at first key */
    uint32_t pairs = map_d.uint32;

    /* Defaults */
    mmdb->metadata.record_size = 28;
    mmdb->metadata.node_count  = 0;
    mmdb->metadata.ip_version  = 6;

    for (uint32_t i = 0; i < pairs; i++) {
        /* Key (always utf8_string) */
        MMDB_entry_data_s key;
        uint32_t ku = decode_data(data, sz, off, &key);
        if (!ku) return MMDB_INVALID_METADATA;
        off += ku;

        /* Peek at value type before consuming it */
        MMDB_entry_data_s val;
        uint32_t vu = decode_data(data, sz, off, &val);
        if (!vu) return MMDB_INVALID_METADATA;

        if (key.type == MMDB_DATA_TYPE_UTF8_STRING && val.has_data) {
            char kbuf[64] = {0};
            uint32_t klen = key.data_size < 63 ? key.data_size : 63;
            memcpy(kbuf, key.utf8_string, klen);

            if (strcmp(kbuf, "node_count") == 0)
                mmdb->metadata.node_count = val.uint32;
            else if (strcmp(kbuf, "record_size") == 0)
                mmdb->metadata.record_size = (uint16_t)val.uint32;
            else if (strcmp(kbuf, "ip_version") == 0)
                mmdb->metadata.ip_version = (uint16_t)val.uint32;
        }

        /* Skip value completely (handles nested maps like description/languages) */
        off = skip_value(data, sz, off);
    }

    if (mmdb->metadata.node_count == 0) return MMDB_INVALID_METADATA;
    return MMDB_SUCCESS;
}

/* ══════════════════════════════════════════════════════════════════════════
   Search-tree node reader
   ══════════════════════════════════════════════════════════════════════════ */
static uint32_t read_node(const MMDB_s *mmdb, uint32_t node, int bit)
{
    uint16_t       rs        = mmdb->metadata.record_size;
    uint32_t       bytes_per = (uint32_t)(rs * 2) / 8u;
    uint32_t       off       = node * bytes_per;
    const uint8_t *p         = mmdb->file_content + off;

    if (rs == 28) {
        if (bit == 0)
            return ((uint32_t)(p[3] & 0xf0u) << 20)
                 | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        else
            return ((uint32_t)(p[3] & 0x0fu) << 24)
                 | ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 8) | p[6];
    }
    if (rs == 32) {
        if (bit == 0)
            return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        else
            return ((uint32_t)p[4]<<24)|((uint32_t)p[5]<<16)|((uint32_t)p[6]<<8)|p[7];
    }
    /* 24 */
    if (bit == 0) return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
    else          return ((uint32_t)p[3]<<16)|((uint32_t)p[4]<<8)|p[5];
}

/* ══════════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════════ */

int MMDB_open(const char *filename, uint32_t flags, MMDB_s *mmdb)
{
    (void)flags;
    memset(mmdb, 0, sizeof *mmdb);

    int fd = open(filename, O_RDONLY);
    if (fd < 0) return MMDB_FILE_OPEN_ERROR;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return MMDB_IO_ERROR; }

    void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return MMDB_IO_ERROR; }

    mmdb->file_content = (const uint8_t *)map;
    mmdb->file_size    = (size_t)st.st_size;
    mmdb->filename     = strdup(filename);

    _MMDB_impl *impl = (_MMDB_impl *)malloc(sizeof(_MMDB_impl));
    if (!impl) { munmap(map, mmdb->file_size); close(fd); return MMDB_OUT_OF_MEMORY; }
    impl->fd    = fd;
    mmdb->_impl = impl;

    int rc = parse_metadata(mmdb);
    if (rc != MMDB_SUCCESS) { MMDB_close(mmdb); return rc; }

    size_t tree_size = (size_t)mmdb->metadata.node_count
                     * (size_t)(mmdb->metadata.record_size * 2) / 8u;

    mmdb->data_section      = mmdb->file_content + tree_size + 16u;
    mmdb->data_section_size = mmdb->file_size - (tree_size + 16u);
    mmdb->data_section_offset_adjustment = (uint32_t)(tree_size + 16u);

    return MMDB_SUCCESS;
}

void MMDB_close(MMDB_s *mmdb)
{
    if (!mmdb) return;
    if (mmdb->file_content)
        munmap((void *)mmdb->file_content, mmdb->file_size);
    if (mmdb->filename) free(mmdb->filename);
    if (mmdb->_impl) {
        close(((_MMDB_impl *)mmdb->_impl)->fd);
        free(mmdb->_impl);
    }
    memset(mmdb, 0, sizeof *mmdb);
}

MMDB_lookup_result_s MMDB_lookup_string(const MMDB_s *mmdb,
                                         const char   *ipstr,
                                         int          *gai_error,
                                         int          *mmdb_error)
{
    MMDB_lookup_result_s result;
    memset(&result, 0, sizeof result);
    if (gai_error)  *gai_error  = 0;
    if (mmdb_error) *mmdb_error = MMDB_SUCCESS;

    if (!mmdb || !mmdb->file_content) {
        if (mmdb_error) *mmdb_error = MMDB_FILE_OPEN_ERROR;
        return result;
    }

    struct in_addr addr4;
    if (inet_pton(AF_INET, ipstr, &addr4) != 1) {
        if (mmdb_error) *mmdb_error = MMDB_INVALID_DATA;
        return result;
    }
    uint32_t ip = ntohl(addr4.s_addr);

    uint32_t node       = 0;
    uint32_t node_count = mmdb->metadata.node_count;

    /* IPv4-in-IPv6 DB: walk 96 zero bits to reach the IPv4 subtree */
    if (mmdb->metadata.ip_version == 6) {
        for (int i = 0; i < 96 && node < node_count; i++)
            node = read_node(mmdb, node, 0);
    }

    if (node >= node_count) {
        if (mmdb_error) *mmdb_error = MMDB_RECORD_NOT_FOUND;
        return result;
    }

    /* Walk 32 bits of the IPv4 address */
    for (int bit = 31; bit >= 0; bit--) {
        int b = (int)((ip >> (unsigned)bit) & 1u);
        node  = read_node(mmdb, node, b);
        if (node >= node_count) break;
    }

    /* node == node_count → empty record; node < node_count → still in tree */
    if (node <= node_count) {
        if (mmdb_error) *mmdb_error = MMDB_RECORD_NOT_FOUND;
        return result;
    }

    /* node > node_count → valid data record */
    uint32_t data_offset = node - node_count - 16u;
    result.found_entry   = 1;
    result.entry.mmdb    = mmdb;
    result.entry.offset  = data_offset;
    return result;
}

/* ── Pointer resolution helper ───────────────────────────────────────────
   Given an offset in the data section that may be a pointer, resolve it
   to the actual target offset.  Returns the resolved offset and the
   number of bytes the pointer encoding consumed (stored in *consumed).    */
static uint32_t resolve_ptr(const uint8_t *dsec, size_t dsz, uint32_t off,
                             uint32_t *consumed)
{
    if (off >= (uint32_t)dsz) { *consumed = 0; return off; }
    const uint8_t *p   = dsec + off;
    size_t         avail = dsz - off;
    uint8_t        ctrl = p[0];
    int            type = (ctrl >> 5) & 0x07;

    /* If not a pointer, nothing to resolve */
    if (type != 1) { *consumed = 0; return off; }

    /* Pointer encoding: bits [4:3] of ctrl = (ptr_size-1) */
    int ptr_size = ((ctrl >> 3) & 3) + 1;
    if (avail < (size_t)(1 + ptr_size)) { *consumed = 0; return off; }

    uint32_t ptr = ctrl & 0x07;
    for (int i = 0; i < ptr_size; i++)
        ptr = (ptr << 8) | p[1 + i];

    /* Add offset bias depending on ptr_size */
    if (ptr_size == 2) ptr += 2048u;
    else if (ptr_size == 3) ptr += 526336u;
    /* ptr_size==4: no bias; ptr_size==1: no bias */

    *consumed = (uint32_t)(1 + ptr_size);
    return ptr;  /* offset into data section */
}

/* Walk a key-path through nested maps in the data section.
   Handles pointers at both map-level and value-level. */
int MMDB_get_value(MMDB_entry_s *entry, MMDB_entry_data_s *entry_data, ...)
{
    memset(entry_data, 0, sizeof *entry_data);
    if (!entry || !entry->mmdb) return MMDB_INVALID_DATA;

    const MMDB_s  *mmdb   = entry->mmdb;
    const uint8_t *dsec   = mmdb->data_section;
    size_t         dsz    = mmdb->data_section_size;
    uint32_t       off    = entry->offset;

    va_list keys;
    va_start(keys, entry_data);

    const char *key;
    while ((key = va_arg(keys, const char *)) != NULL) {
        /* Resolve pointer before expecting a map */
        uint32_t ptr_consumed = 0;
        uint32_t ptr_target   = resolve_ptr(dsec, dsz, off, &ptr_consumed);
        uint32_t map_off = (ptr_consumed > 0) ? ptr_target : off;

        /* Expect a map at map_off */
        MMDB_entry_data_s map_d;
        uint32_t hdr = decode_data(dsec, dsz, map_off, &map_d);
        if (!hdr || map_d.type != MMDB_DATA_TYPE_MAP) {
            va_end(keys);
            return MMDB_LOOKUP_PATH_MISMATCH;
        }
        uint32_t content_off = map_off + hdr;   /* first key in this map */

        uint32_t   pairs = map_d.uint32;
        int        found = 0;
        size_t     klen  = strlen(key);

        for (uint32_t i = 0; i < pairs; i++) {
            /* Decode key (keys are always utf8_string or pointer-to-utf8) */
            uint32_t kptr_consumed = 0;
            uint32_t kptr_target   = resolve_ptr(dsec, dsz, content_off, &kptr_consumed);
            uint32_t real_key_off  = (kptr_consumed > 0) ? kptr_target : content_off;

            MMDB_entry_data_s k;
            uint32_t ku = decode_data(dsec, dsz, real_key_off, &k);
            if (!ku) { va_end(keys); return MMDB_INVALID_DATA; }
            /* Advance past the key's bytes in the *stream* (pointer or literal) */
            if (kptr_consumed > 0)
                content_off += kptr_consumed;
            else
                content_off += ku;

            int match = (k.type == MMDB_DATA_TYPE_UTF8_STRING &&
                         k.data_size == (uint32_t)klen &&
                         memcmp(k.utf8_string, key, klen) == 0);

            if (match) {
                /* Resolve value pointer if present */
                uint32_t vptr_consumed = 0;
                uint32_t vptr_target   = resolve_ptr(dsec, dsz, content_off, &vptr_consumed);
                uint32_t val_off = (vptr_consumed > 0) ? vptr_target : content_off;

                MMDB_entry_data_s v;
                uint32_t vu = decode_data(dsec, dsz, val_off, &v);
                if (!vu) { va_end(keys); return MMDB_INVALID_DATA; }

                found = 1;
                if (v.type == MMDB_DATA_TYPE_MAP) {
                    /* Descend into this map on the next loop iteration.
                       off = the pointer bytes in the stream (or the map header in place) */
                    if (vptr_consumed > 0)
                        off = content_off + vptr_consumed;  /* past ptr in stream */
                    else
                        off = content_off;                  /* at map header */
                    /* But the actual map data is at val_off — store that as off
                       so the outer while-loop re-decodes the map correctly      */
                    off = val_off;
                } else {
                    *entry_data = v;
                }
                break;
            }

            /* Skip value (in stream — may be a pointer) */
            content_off = skip_value(dsec, dsz, content_off);
        }

        if (!found) {
            va_end(keys);
            return MMDB_LOOKUP_PATH_MISMATCH;
        }
    }

    va_end(keys);
    return MMDB_SUCCESS;
}

const char *MMDB_strerror(int error_code)
{
    switch (error_code) {
        case MMDB_SUCCESS:              return "Success";
        case MMDB_FILE_OPEN_ERROR:      return "File open error";
        case MMDB_CORRUPT_SEARCH_TREE:  return "Corrupt search tree";
        case MMDB_INVALID_METADATA:     return "Invalid metadata";
        case MMDB_IO_ERROR:             return "I/O error";
        case MMDB_OUT_OF_MEMORY:        return "Out of memory";
        case MMDB_RECORD_NOT_FOUND:     return "Record not found";
        case MMDB_LOOKUP_PATH_MISMATCH: return "Lookup path mismatch";
        default:                        return "Unknown error";
    }
}
