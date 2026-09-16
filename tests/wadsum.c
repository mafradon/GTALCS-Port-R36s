/* Dump name,size,crc32 of every entry's DECRYPTED payload, without writing
 * 2 GB to disk — so the C extractor can be compared to the Python one. */
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "wad_extract.h"
/* reach into the implementation for the parsed table */
#define main wad_extract_unused_main
#include "wad_extract.c"
#undef main

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wadsum ARCHIVE DICT\n"); return 2; }
    FILE *f; wad_ent *ents; unsigned n; char err[256];
    if (wad_open(argv[1], &f, &ents, &n, err, sizeof err) != 0) { fprintf(stderr, "%s\n", err); return 1; }
    unsigned named = 0;
    char *blob = attach_names(argv[2], ents, n, &named, err, sizeof err);
    if (!blob) { fprintf(stderr, "%s\n", err); return 1; }
    fprintf(stderr, "entries=%u named=%u\n", n, named);
    unsigned char *buf = malloc(WAD_CHUNK);
    char rel[768];
    for (unsigned i = 0; i < n; i++) {
        if (!ents[i].size || !ents[i].off) continue;
        if (ents[i].name) sanitise(ents[i].name, rel, sizeof rel);
        else snprintf(rel, sizeof rel, "_unnamed/file_%05u", i);
        unsigned long long off = ents[i].off, left = ents[i].size;
        uLong c = crc32(0, NULL, 0);
        fseeko(f, (off_t)off, SEEK_SET);
        while (left) {
            size_t want = left < WAD_CHUNK ? (size_t)left : WAD_CHUNK;
            size_t got = fread(buf, 1, want, f);
            if (got != want) { fprintf(stderr, "SHORT READ %s\n", rel); return 1; }
            wad_decrypt(buf, got, off);
            c = crc32(c, buf, (uInt)got);
            off += got; left -= got;
        }
        printf("%08lx %llu %s\n", (unsigned long)c, ents[i].size, rel);
    }
    return 0;
}
