#!/usr/bin/env python3
"""Reader for `skills rawsave` files (format CMRAW001).

  python skillraw.py capture.bin             # one line per capture + the byte ranges that changed during the call
  python skillraw.py capture.bin --dump 5 S  # hex dump of capture #5, dump "S" (a2, S, o160, a1; add _after for the after-call dump)

As a module:  from skillraw import load;  caps = load(path)
Each capture: dict(idx, t_us, tid, key0, base=[a2,S,o160,a1], sz=[..], valid_before=[..], valid_after=[..], has_after,
                   before=[bytes x4], after=[bytes x4] or None)
"""
import struct
import sys

NAMES = ["a2", "S", "o160", "a1"]


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"CMRAW001":
        raise ValueError("not a CMRAW001 file")
    version, nslots, rec_hdr, _ = struct.unpack_from("<IIII", data, 8)
    if version != 1 or rec_hdr != 112:
        raise ValueError("unsupported version/header size")
    off = 24
    caps = []
    for _ in range(nslots):
        idx, t_us, tid, key0 = struct.unpack_from("<QqII", data, off)
        base = list(struct.unpack_from("<4Q", data, off + 24))
        sz = list(struct.unpack_from("<4I", data, off + 56))
        vb = list(struct.unpack_from("<4I", data, off + 72))
        va = list(struct.unpack_from("<4I", data, off + 88))
        has_after, _pad = struct.unpack_from("<II", data, off + 104)
        off += rec_hdr
        before = []
        for n in sz:
            before.append(data[off:off + n]); off += n
        after = None
        if has_after:
            after = []
            for n in sz:
                after.append(data[off:off + n]); off += n
        caps.append(dict(idx=idx, t_us=t_us, tid=tid, key0=key0, base=base, sz=sz, valid_before=vb, valid_after=va,
                         has_after=bool(has_after), before=before, after=after))
    if off != len(data):
        raise ValueError("trailing bytes: file is corrupt")
    return caps


def changed_ranges(a, b):
    """[(offset, length)] where two equal-length byte strings differ."""
    out, i, n = [], 0, min(len(a), len(b))
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            out.append((i, j - i)); i = j
        else:
            i += 1
    return out


def main(argv):
    caps = load(argv[0])
    if len(argv) >= 4 and argv[1] == "--dump":
        c = next(x for x in caps if x["idx"] == int(argv[2]))
        name = argv[3]
        which = NAMES.index(name.replace("_after", ""))
        blob = (c["after"] if name.endswith("_after") else c["before"])[which]
        for i in range(0, len(blob), 32):
            print("%04X: %s" % (i, " ".join("%02X" % b for b in blob[i:i + 32])))
        return 0
    for c in caps:
        print("[%d] t=%dus tid=%d key0=%d a2=0x%X S=0x%X o160=0x%X a1=0x%X sizes=%s after=%s" % (
            c["idx"], c["t_us"], c["tid"], c["key0"], *c["base"], c["sz"], c["has_after"]))
        if c["has_after"]:
            for k, nm in enumerate(NAMES):
                rs = changed_ranges(c["before"][k], c["after"][k])
                if rs:
                    print("    %s changed: %s" % (nm, ", ".join("+0x%X(%d)" % r for r in rs[:12]) + (" ..." if len(rs) > 12 else "")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
