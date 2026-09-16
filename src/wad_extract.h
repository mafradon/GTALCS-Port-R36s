/* wad_extract.h — extraction of Rockstar 'DAWL' WAD archives (GTA:LCS .obb).
 *
 * The engine can read the OBB in place, but decrypting every asset read on a
 * handheld costs CPU the game does not have to spare, so the installer unpacks
 * once to plain files and the port then runs from those.
 */
#ifndef WAD_EXTRACT_H
#define WAD_EXTRACT_H

#include <stddef.h>

/* Called as extraction proceeds so the caller can draw a progress bar.  `name`
 * is the file just written.  Called at most a few times per second. */
typedef void (*wad_progress_fn)(void *ud,
                                unsigned            files_done,
                                unsigned            files_total,
                                unsigned long long  bytes_done,
                                unsigned long long  bytes_total,
                                const char         *name);

typedef struct {
    wad_progress_fn progress;
    void           *ud;
} wad_opts;

/* Total decrypted payload size, for a free-space check before committing.
 * Returns -1 on error. */
long long wad_payload_bytes(const char *archive, char *err, size_t errsz);

/* Extract `archive` into `outdir`, naming entries from `dict_path` (one path
 * per line; the archive stores only a hash of each name).  Existing files are
 * overwritten, which is how the patch OBB is applied over the main one.
 * Returns the number of files written, or -1 on error with `err` filled. */
long wad_extract(const char *archive, const char *dict_path, const char *outdir,
                 const wad_opts *opts, char *err, size_t errsz);

#endif
