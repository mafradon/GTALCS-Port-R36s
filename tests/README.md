# Host-side verification harnesses

These run natively on the build machine — no device needed. Each of them found a
real bug before the code it tests ever reached hardware, which is the whole
argument for keeping them.

| File | Verifies | Found |
|---|---|---|
| `wadsum.c` | the C WAD extractor against `scripts/obb_extract.py` | nothing — it matched all 10133 entries exactly, which is the point |
| `wadrun.c` | the extractor's file-writing path (paths, mkdir -p, overwrite) | — |
| `cantest.c` | the canary allocator in `src/guard_alloc.c` | a header size only correct on 32-bit ARM; a double-free marker stored in freed memory where glibc's tcache overwrites it |
| `ctypetest.c` | `android_ctype_table`'s classifications through the engine's own `ptr[c+1]` indexing | confirmed the `_S`/`_C` correction |

## Running them

```sh
S=src                       # from the project root
gcc -O2 -o /tmp/wadsum tests/wadsum.c -I $S -lz
gcc -O2 -o /tmp/wadrun tests/wadrun.c $S/wad_extract.c -I $S -lz
gcc -O1 -g -o /tmp/cantest tests/cantest.c          # includes guard_alloc.c directly
gcc -O1    -o /tmp/ctypetest tests/ctypetest.c      # needs ctype_tbl.inc, see below
```

`wadsum` prints `crc32 size name` per entry; diff it against the same dump from
`obb_extract.py` to prove the two implementations agree:

```sh
/tmp/wadsum Original-Files/main.*.obb scripts/obb_dictionary.txt | sort > /tmp/c.txt
# ...produce the Python side the same way, then:
diff /tmp/c.txt /tmp/py.txt        # must be empty
```

`ctypetest.c` expects `ctype_tbl.inc`, which is just the `android_ctype_table`
array lifted out of `src/main.c` — regenerate it by copying the declaration
through the closing `};`.

**The rule these exist to enforce:** a broken instrument is worse than none. Each
of them asserts its own invariants and fails loudly, because a confidently wrong
crash report costs a whole debugging cycle — this port has paid that twice.
