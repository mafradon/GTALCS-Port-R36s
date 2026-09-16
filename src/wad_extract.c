/* wad_extract.c — Rockstar 'DAWL' WAD (GTA:LCS .obb) -> plain files.
 *
 * FORMAT, decoded from libGTALcs.so (WadArchive::OpenWad, WadHeader/WadDir/
 * WadFatEntry::Deserialise, SerialiseEncryptDecryptBuffer) and cross-checked
 * against scripts/obb_extract.py, which produced the tree this port has been
 * running from:
 *
 *   @0      u32 'DAWL', u32 version, i32 field
 *           then pad to the next 2048-byte boundary: pos += 2048 - (pos & 0x7FF)
 *   @0x800  u32 numDirs, dirs[numDirs] { i32 parent, u32 name_off }   (8 B)
 *           u32 numFat,  fat[numFat]  { u32 hash_key, u32 name_off,
 *                                       u32 dir_idx,  u32 data_crc,
 *                                       u64 offset,   u64 size,
 *                                       u64 size_again, u64 zero }    (48 B)
 *           u32 names_size, then the name pool if names_size > 0
 *
 * EVERYTHING — header, tables and payload alike — is XOR obfuscated by
 * ABSOLUTE file offset: even offsets ^ 0xAF, odd offsets ^ 0x66.  Chunked reads
 * must therefore keep track of the true offset, not the buffer index.
 *
 * NAMES: both shipped OBBs have names_size == 0, i.e. no name pool, so entries
 * are identified only by `hash_key == ~crc32(lowercase(path))`.  Rather than
 * repeat the Python extractor's pool-location heuristics on-device, the port
 * ships the resolved path list (obb_dictionary.txt) and matches by CRC here.
 * Measured: that resolves 10133/10133 entries in main.17 and 13/13 in
 * patch.15 — every single file, no heuristics, no fallbacks needed.
 *
 * NOT a checksum: the `data_crc` field is not crc32 (nor ~crc32, nor adler32)
 * of the payload in either encrypted or decrypted form — all four were tested
 * and none match.  Do not use it to verify a file; integrity here rests on the
 * declared size being readable in full.
 */

#define _FILE_OFFSET_BITS 64      /* the main OBB is 1.99 GB — fseeko needs this */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <zlib.h>

#include "wad_extract.h"

#define WAD_CHUNK (1u << 20)

typedef struct {
    uint32_t           key;
    unsigned long long off, size;
    const char        *name;          /* into the dictionary block, or NULL */
} wad_ent;

static void seterr(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* XOR by ABSOLUTE offset parity. */
static void wad_decrypt(unsigned char *b, size_t n, unsigned long long off) {
    for (size_t i = 0; i < n; i++)
        b[i] ^= ((off + i) & 1) ? 0x66 : 0xAF;
}

static int read_dec(FILE *f, unsigned long long off, void *buf, size_t n) {
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    if (fread(buf, 1, n, f) != n)             return -1;
    wad_decrypt((unsigned char *)buf, n, off);
    return 0;
}

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static unsigned long long rd64(const unsigned char *p) {
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32);
}

/* mkdir -p for the directory part of `path` (which is modified then restored). */
static int make_parents(char *path) {
    for (char *s = path + 1; *s; s++) {
        if (*s != '/') continue;
        *s = '\0';
        if (mkdir(path, 0775) != 0 && errno != EEXIST) { *s = '/'; return -1; }
        *s = '/';
    }
    return 0;
}

/* Keep the archive's own casing (the engine case-folds; the tree this port
 * runs from uses these names verbatim), but refuse anything that could escape
 * the destination directory. */
static void sanitise(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    int seg_start = 1;
    for (const char *p = in; *p && o + 1 < outsz; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/') {
            if (seg_start) continue;                  /* no empty segments */
            seg_start = 1;
        } else {
            if (seg_start && c == '.' && (p[1] == '.' || p[1] == '/' || !p[1]))
                continue;                             /* no "." / ".." */
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '_';
            seg_start = 0;
        }
        out[o++] = c;
    }
    while (o && (out[o - 1] == '/' || out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = '\0';
    if (!out[0]) snprintf(out, outsz, "unnamed");
}

static int cmp_key(const void *a, const void *b) {
    uint32_t x = ((const wad_ent *)a)->key, y = ((const wad_ent *)b)->key;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Parse header + FAT.  On success *out_ents / *out_n own memory the caller
 * frees.  Returns 0, or -1 with err filled. */
static int wad_open(const char *archive, FILE **out_f, wad_ent **out_ents,
                    unsigned *out_n, char *err, size_t errsz) {
    FILE *f = fopen(archive, "rb");
    if (!f) { seterr(err, errsz, "cannot open %s: %s", archive, strerror(errno)); return -1; }

    unsigned char hdr[12];
    if (read_dec(f, 0, hdr, sizeof hdr) != 0) {
        seterr(err, errsz, "%s: too short to be a WAD", archive); fclose(f); return -1;
    }
    if (memcmp(hdr, "DAWL", 4) != 0) {
        seterr(err, errsz, "%s: not a DAWL archive", archive); fclose(f); return -1;
    }

    unsigned long long pos = 12;
    pos += 2048 - (pos & 0x7FF);

    unsigned char u32b[4];
    if (read_dec(f, pos, u32b, 4) != 0) goto trunc;
    uint32_t ndirs = rd32(u32b);
    pos += 4;
    if (ndirs > (1u << 22)) { seterr(err, errsz, "%s: implausible dir count %u", archive, ndirs); fclose(f); return -1; }
    pos += 8ull * ndirs;                       /* dirs are unused: the dictionary carries full paths */

    if (read_dec(f, pos, u32b, 4) != 0) goto trunc;
    uint32_t nfat = rd32(u32b);
    pos += 4;
    if (nfat == 0 || nfat > (1u << 22)) {
        seterr(err, errsz, "%s: implausible file count %u", archive, nfat); fclose(f); return -1;
    }

    unsigned char *raw = malloc(48ull * nfat);
    wad_ent      *ents = calloc(nfat, sizeof *ents);
    if (!raw || !ents) { seterr(err, errsz, "out of memory for %u entries", nfat); free(raw); free(ents); fclose(f); return -1; }
    if (read_dec(f, pos, raw, 48ull * nfat) != 0) { free(raw); free(ents); goto trunc; }

    for (uint32_t i = 0; i < nfat; i++) {
        const unsigned char *e = raw + 48ull * i;
        ents[i].key  = rd32(e);
        ents[i].off  = rd64(e + 16);
        ents[i].size = rd64(e + 24);
    }
    free(raw);

    *out_f = f; *out_ents = ents; *out_n = nfat;
    return 0;
trunc:
    seterr(err, errsz, "%s: truncated archive", archive);
    fclose(f);
    return -1;
}

long long wad_payload_bytes(const char *archive, char *err, size_t errsz) {
    FILE *f; wad_ent *ents; unsigned n;
    if (wad_open(archive, &f, &ents, &n, err, errsz) != 0) return -1;
    unsigned long long tot = 0;
    for (unsigned i = 0; i < n; i++)
        if (ents[i].size && ents[i].off) tot += ents[i].size;
    free(ents); fclose(f);
    return (long long)tot;
}

/* Load the dictionary and attach a name to every entry whose hash matches. */
static char *attach_names(const char *dict_path, wad_ent *ents, unsigned n,
                          unsigned *out_named, char *err, size_t errsz) {
    FILE *d = fopen(dict_path, "rb");
    if (!d) { seterr(err, errsz, "cannot open %s: %s", dict_path, strerror(errno)); return NULL; }
    if (fseeko(d, 0, SEEK_END) != 0) { fclose(d); seterr(err, errsz, "%s: not seekable", dict_path); return NULL; }
    long long dsz = (long long)ftello(d);
    rewind(d);
    if (dsz <= 0 || dsz > (64 << 20)) { fclose(d); seterr(err, errsz, "%s: bad size", dict_path); return NULL; }

    char *blob = malloc((size_t)dsz + 1);
    if (!blob) { fclose(d); seterr(err, errsz, "out of memory for the dictionary"); return NULL; }
    if (fread(blob, 1, (size_t)dsz, d) != (size_t)dsz) {
        free(blob); fclose(d); seterr(err, errsz, "%s: short read", dict_path); return NULL;
    }
    fclose(d);
    blob[dsz] = '\0';

    /* Index by hash so each dictionary line is one binary search, not a scan. */
    wad_ent **bykey = malloc(n * sizeof *bykey);
    if (!bykey) { free(blob); seterr(err, errsz, "out of memory"); return NULL; }
    for (unsigned i = 0; i < n; i++) bykey[i] = &ents[i];
    /* sort an index array by key */
    qsort(ents, n, sizeof *ents, cmp_key);
    free(bykey);

    unsigned named = 0;
    char *p = blob;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        char *end = p;
        while (*p == '\n' || *p == '\r') *p++ = '\0';
        *end = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) continue;

        char low[512];
        size_t L = strlen(line);
        if (L >= sizeof low) continue;
        for (size_t i = 0; i <= L; i++) low[i] = (char)tolower((unsigned char)line[i]);

        uint32_t key = (uint32_t)~crc32(0, (const Bytef *)low, (uInt)L);

        unsigned lo = 0, hi = n;                   /* lower_bound on sorted ents */
        while (lo < hi) { unsigned mid = lo + (hi - lo) / 2;
                          if (ents[mid].key < key) lo = mid + 1; else hi = mid; }
        for (unsigned i = lo; i < n && ents[i].key == key; i++)
            if (!ents[i].name) { ents[i].name = line; named++; break; }
    }
    *out_named = named;
    return blob;                                    /* caller frees; names point into it */
}

long wad_extract(const char *archive, const char *dict_path, const char *outdir,
                 const wad_opts *opts, char *err, size_t errsz) {
    FILE *f; wad_ent *ents; unsigned n;
    if (wad_open(archive, &f, &ents, &n, err, errsz) != 0) return -1;

    unsigned named = 0;
    char *blob = attach_names(dict_path, ents, n, &named, err, errsz);
    if (!blob) { free(ents); fclose(f); return -1; }

    unsigned long long total_bytes = 0;
    for (unsigned i = 0; i < n; i++)
        if (ents[i].size && ents[i].off) total_bytes += ents[i].size;

    unsigned char *buf = malloc(WAD_CHUNK);
    if (!buf) { seterr(err, errsz, "out of memory"); free(blob); free(ents); fclose(f); return -1; }

    long  written = 0;
    unsigned long long done_bytes = 0;
    char path[1024], rel[768];

    for (unsigned i = 0; i < n; i++) {
        if (!ents[i].size || !ents[i].off) continue;

        if (ents[i].name) sanitise(ents[i].name, rel, sizeof rel);
        else              snprintf(rel, sizeof rel, "_unnamed/file_%05u", i);

        if ((size_t)snprintf(path, sizeof path, "%s/%s", outdir, rel) >= sizeof path) {
            seterr(err, errsz, "path too long: %s/%s", outdir, rel); goto fail;
        }
        if (make_parents(path) != 0) {
            seterr(err, errsz, "cannot create directory for %s: %s", rel, strerror(errno)); goto fail;
        }

        FILE *w = fopen(path, "wb");
        if (!w) { seterr(err, errsz, "cannot write %s: %s", rel, strerror(errno)); goto fail; }

        unsigned long long off = ents[i].off, left = ents[i].size;
        if (fseeko(f, (off_t)off, SEEK_SET) != 0) {
            fclose(w); seterr(err, errsz, "%s: seek failed at %llu", archive, off); goto fail;
        }
        while (left) {
            size_t want = left < WAD_CHUNK ? (size_t)left : WAD_CHUNK;
            size_t got  = fread(buf, 1, want, f);
            if (got != want) {
                fclose(w);
                seterr(err, errsz, "%s: truncated reading %s (%llu bytes short)",
                       archive, rel, (unsigned long long)(left - got));
                goto fail;
            }
            wad_decrypt(buf, got, off);
            if (fwrite(buf, 1, got, w) != got) {
                fclose(w);
                seterr(err, errsz, "write failed for %s: %s (disk full?)", rel, strerror(errno));
                goto fail;
            }
            off += got; left -= got; done_bytes += got;
        }
        if (fclose(w) != 0) {
            seterr(err, errsz, "write failed closing %s: %s (disk full?)", rel, strerror(errno));
            goto fail;
        }
        written++;

        if (opts && opts->progress)
            opts->progress(opts->ud, (unsigned)written, n, done_bytes, total_bytes, rel);
    }

    free(buf); free(blob); free(ents); fclose(f);
    return written;
fail:
    free(buf); free(blob); free(ents); fclose(f);
    return -1;
}
