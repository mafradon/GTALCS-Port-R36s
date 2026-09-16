#!/usr/bin/env python3
"""
obb_extract.py — extract Rockstar 'DAWL' WAD archives (.obb) to plain files.

Format, as decoded from libGTALcs.so (WadArchive::OpenWad, WadHeader/WadDir/
WadFatEntry::Deserialise, Wad_GetPaddingRequired, SerialiseEncryptDecryptBuffer):

  @0     u32 magic 'DAWL', u32 version, i32 field
         pad to next 2048-byte boundary:  pos += 2048 - (pos & 0x7FF)
  @0x800 u32 numDirs
         dirs[numDirs]: {i32 parent_idx, u32 name_off}      (8 B each)
         u32 numFat
         fat[numFat]:   {u32 hash_key, u32 name_off, u32 dir_idx, u32 data_crc,
                         u64 offset, u64 end, u64 size, u64 0}   (48 B each)
         u32 names_size (+ packed NUL-separated name pool, if names_size>0)
  payload: each entry's bytes at `offset`, XOR'd with {0xAF,0x66} selected by
           ABSOLUTE file-offset parity.

File names are resolved from the name pool when present; otherwise the packed
pool is located by the tiling constraint (consecutive name_offs are exactly one
string + NUL apart) and, failing that, by CRC32 dictionary attack
(hash_key = ~crc32(lowercase(path))).  Unresolved entries get index names.

Usage: obb_extract.py MAIN_OBB PATCH_OBB OUT_DIR [--hashdict DICT]
"""
import bisect, os, re, struct, sys, zlib

XOR_A = b'\xaf\x66'
XOR_B = b'\x66\xaf'
CHUNK = 1 << 23  # 8 MiB; even -> parity pattern stable within a chunk

def xor_range(buf, base):
    """XOR buf in place-equivalent; base = absolute file offset of buf[0]."""
    ks = (XOR_A if base % 2 == 0 else XOR_B)
    return bytes(x ^ y for x, y in zip(buf, ks * (len(buf) // 2 + 1)))[:len(buf)]

def stream(f, pos, n):
    f.seek(pos)
    out = bytearray()
    while n > 0:
        c = f.read(min(CHUNK, n))
        if not c:
            break
        out += xor_range(c, pos)
        pos += len(c)
        n -= len(c)
    return bytes(out)

def stream_to(f, pos, n, w):
    """Decrypted copy to a file object, chunked (payloads reach 275 MB)."""
    f.seek(pos)
    while n > 0:
        c = f.read(min(CHUNK, n))
        if not c:
            break
        w.write(xor_range(c, pos))
        pos += len(c)
        n -= len(c)

class Archive:
    def __init__(self, path):
        self.path = path
        f = self.f = open(path, 'rb')
        f.seek(0, 2)
        self.size = f.tell()
        hdr = stream(f, 0, 12)
        if hdr[:4] != b'DAWL':
            raise SystemExit(f'{path}: not DAWL ({hdr[:4]!r})')
        pos = 12
        pos += 2048 - (pos & 0x7FF)
        nd = struct.unpack('<I', stream(f, pos, 4))[0]
        pos += 4
        raw = stream(f, pos, 8 * nd) if nd else b''
        self.dirs = [struct.unpack_from('<iI', raw, 8 * i) for i in range(nd)]
        pos += 8 * nd
        nf = struct.unpack('<I', stream(f, pos, 4))[0]
        pos += 4
        raw = stream(f, pos, 48 * nf)
        self.fat = [struct.unpack_from('<4I4Q', raw, 48 * i) for i in range(nf)]
        pos += 48 * nf
        self.names_size = struct.unpack('<I', stream(f, pos, 4))[0]
        self.names_at = pos + 4
        self.names = stream(f, self.names_at, self.names_size) if self.names_size else b''

    def pool_candidates(self):
        """Yield plausible name-pool bases: tiling-constraint scan over the
        header region.  Consecutive distinct name_offs must be exactly one
        NUL-terminated string apart."""
        offs = sorted({x[1] for x in self.fat} | {d[1] for d in self.dirs})
        if len(offs) < 4:
            return
        probe = offs[:24]
        gaps = [(probe[i + 1] - probe[i]) for i in range(len(probe) - 1)]
        # window covering name range
        lo, hi = 0x1000, min(self.size, 0x600000)
        win = stream(self.f, lo, hi - lo)
        o0 = probe[0]
        for B in range(lo, hi):
            p = B + o0 - lo
            if p < 0 or p >= len(win):
                continue
            b = win[p]
            if not (0x20 <= b <= 0x7e) or win[p + gaps[0] - 1:p + gaps[0]] != b'\x00':
                continue
            if all(0 < (g := g2) and win[p + (probe[i + 1] - o0) - 1] == 0
                   and win[p + (probe[i + 1] - o0) - 2:p + (probe[i + 1] - o0) - 1] != b'\x00'
                   for i, g2 in enumerate(gaps[:6])):
                yield B

    def name_from_pool(self, B, off):
        p = B + off - (self.names_at if False else 0)
        buf = stream(self.f, B + off, 64)
        e = buf.find(b'\x00')
        if e < 0 or e > 60 or any(c < 0x20 or c > 0x7e for c in buf[:e]):
            return None
        return buf[:e].decode('latin1')

    def dir_path(self, B, d):
        parts = []
        seen = 0
        while d != -1 and d < len(self.dirs) and seen < 32:
            parent, noff = self.dirs[d]
            n = self.name_from_pool(B, noff) if B else ''
            parts.append(n or f'dir{d}')
            d = parent
            seen += 1
        return '/'.join(reversed(parts))

    def hash_dictionary(self, extra_paths):
        """CRC dictionary attack: key == ~crc32(lowercase(path))."""
        keys = [x[0] for x in self.fat]
        out = {}
        for p in extra_paths:
            k = (~zlib.crc32(p.lower().encode('latin1', 'replace'))) & 0xffffffff
            i = bisect.bisect_left(keys, k)
            if i < len(keys) and keys[i] == k:
                out[i] = p
        return out

def dict_corpus(extra):
    """Candidate paths for the hash dictionary."""
    paths = set()
    for s in extra:
        s = s.strip().lower()
        if not s:
            continue
        paths.add(s)
    return paths

def sanitize(p):
    p = re.sub(r'[\\]', '/', p).lstrip('/')
    p = re.sub(r'[^\x20-\x7e]', '_', p)
    p = '/'.join(seg.strip(' .') or '_' for seg in p.split('/') if seg not in ('', '.', '..'))
    return p or 'unnamed'

def main():
    main_obb, patch_obb, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    dicts = set()
    if '--hashdict' in sys.argv:
        for line in open(sys.argv[sys.argv.index('--hashdict') + 1], errors='replace'):
            line = line.strip()
            if line:
                dicts.add(line)
    # a few known paths (observed fopen attempts / engine strings)
    dicts |= dict_corpus('''
text/english.gxt text/german.gxt text/french.gxt text/italian.gxt text/spanish.gxt
data/main.scm data/object.dat data/ped.dat data/weapon.dat data/surface.dat
data/water.dat data/pedgrp.dat data/gta_vc.dat
models/generic.txd models/fonts.txd models/particle.txd models/fronten1.txd
models/fronten2.txd models/indust.img models/commer.img
'''.split())

    written = {}
    for which, path in (('main', main_obb), ('patch', patch_obb)):
        arc = Archive(path)
        print(f'[{which}] {path}: {len(arc.fat)} files, {arc.size/1e6:.0f} MB, names_size={arc.names_size}', flush=True)

        pool_at = None
        if arc.names_size:
            pool_at = arc.names_at
        else:
            for cand in arc.pool_candidates():
                # full verification
                okall = all(arc.name_from_pool(cand, x[1]) for x in arc.fat[:64])
                if okall:
                    pool_at = cand
                    break
        by_hash = {}
        if pool_at is None:
            by_hash = arc.hash_dictionary(dicts)
        print(f'[{which}] names: pool @ {hex(pool_at) if pool_at else "NOT FOUND"}, {len(by_hash)} hash-resolved', flush=True)

        n = 0
        for i, x in enumerate(arc.fat):
            off, sz = x[4], x[5]
            if sz == 0 or off == 0:
                continue
            if pool_at is not None:
                d = arc.dir_path(pool_at, x[2])
                nme = arc.name_from_pool(pool_at, x[1]) or f'fat{i}'
                name = (d + '/' if d else '') + nme
            elif i in by_hash:
                name = by_hash[i]
            else:
                name = f'file_{i:05d}_{off:x}'
            dest = os.path.join(outdir, sanitize(name))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as w:
                stream_to(arc.f, off, sz, w)
            prev = written.get(sanitize(name))
            written[sanitize(name)] = (which, off, sz)
            n += 1
            if prev:
                print(f'[{which}] OVERWROTE {name}  (was {prev[0]} @ {prev[1]:x} len {prev[2]:x})', flush=True)
            elif n % 500 == 0:
                print(f'[{which}] {n}/{len(arc.fat)}', flush=True)
        print(f'[{which}] wrote {n} files', flush=True)
    # summary
    ov = [k for k, v in written.items() if v[0] == 'patch']
    print(f'TOTAL {len(written)} unique files; {len(ov)} touched by patch')
    for k in sorted(ov):
        print('  patched:', k)

if __name__ == '__main__':
    main()
