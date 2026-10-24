"""Repoint zero-load-address PADDING segments at scratch DRAM.

Why not delete them: esptool write_flash streams the .bin
sequentially and the *file offsets* are what keep the later
flash-mapped segments on their required 64 KB MMU pages.
Why the sanity check: a la=0 segment whose payload is NOT all
zeros is not padding -- refuse to touch it.
"""
import hashlib
import struct
import sys

SCRATCH_BASE = 0x3FC88000  # scratch region sits in the unused DRAM gap
                          # below the image's own .data/.bss segment.
# SCRATCH_LIMIT is computed per-image in main() (the lowest real segment
# load address at or above SCRATCH_BASE).  The previous fixed 32 KB window
# (0x3FC88000..0x3FC90000) was too small for IRAM-DCache-split builds
# whose two zero-addr PADDING segments total > 32 KB (run#129:
# 0x1840 + 0xf64c = 69260 bytes) -- the relocation overflowed, the
# ZERO-ADDR GATE then rejected a perfectly bootable image.
SCRATCH_LIMIT = 0x3FC90000  # fallback only; real limit is computed in main()


def parse(path):
    data = bytearray(open(path, "rb").read())
    if len(data) < 24 or data[0] != 0xE9:
        raise SystemExit("bad magic/size -- not an ESP firmware image")
    n = data[1]
    off = 24
    entries = []
    for i in range(n):
        if off + 8 > len(data):
            raise SystemExit("truncated segment header %d" % i)
        la, ln = struct.unpack_from("<II", data, off)
        if off + 8 + ln > len(data):
            raise SystemExit("segment %d data truncated" % i)
        entries.append((i, off, la, ln))
        off += 8 + ln
    return data, entries


def main(src, dst):
    data, entries = parse(src)
    # Safe upper bound: the lowest real (non-zero) segment load address
    # at or above SCRATCH_BASE.  Segments with a lower load address
    # (flash-XIP DROM at 0x3C000000, etc.) sit far below the scratch
    # region and can never be overlapped.  The relocated all-zero
    # PADDING then occupies only the unused DRAM gap below .data/.bss,
    # which NuttX re-initialises as heap at startup, so the copies are
    # harmless (this is the same safe region the fixed 32 KB window
    # used; we only let it grow to fit larger MMU-page padding such as
    # run#129's 69260 bytes).
    above = [la for _, _, la, _ in entries if la != 0 and la >= SCRATCH_BASE]
    scratch_limit = min(above) if above else (SCRATCH_BASE + 0x20000)
    scratch = SCRATCH_BASE
    fixed = []
    for i, off, la, ln in entries:
        if la != 0:
            continue
        payload = data[off + 8: off + 8 + ln]
        if any(payload):
            raise SystemExit(
                "seg%d la=0 but payload is not all zeros "
                "-- NOT padding, refusing to patch" % i)
        struct.pack_into("<I", data, off, scratch)
        fixed.append((i, ln, scratch))
        scratch += ln
    if not fixed:
        print("no zero-load-address segments; nothing to do")
    else:
        for i, ln, addr in fixed:
            print("  seg%d: load 0x00000000 -> 0x%08x "
                  "(len 0x%x, all zeros)" % (i, addr, ln))
        if scratch > scratch_limit:
            raise SystemExit(
                "scratch overflow 0x%08x > 0x%08x (lowest real seg at/above "
                "base is 0x%08x) -- cannot relocate PADDING without clobbering "
                "a real segment"
                % (scratch, scratch_limit, min(above) if above else 0))
    # Refresh the appended SHA-256 digest (last 32 bytes,
    # hash_appended flag is byte 23) so esptool image_info keeps
    # reporting "Validation Hash: (valid)".
    if data[23] & 1:
        if len(data) < 56:
            raise SystemExit("image too small for an appended digest")
        doff = len(data) - 32
        data[doff:] = hashlib.sha256(bytes(data[:doff])).digest()
        print("  sha256 digest refreshed at 0x%x" % doff)
    open(dst, "wb").write(bytes(data))
    # Re-verify: re-parse the OUTPUT and count zero la segments.
    d2, e2 = parse(dst)
    bad = sum(1 for _, _, la, _ in e2 if la == 0)
    print("verify: segcount=%d zero_load_addr_segments=%d"
          % (len(e2), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.stdout.write(
            "usage: patch_zero_segs.py <in.bin> <out.bin>\n")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
