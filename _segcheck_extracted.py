#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32-S3 firmware image segment checker (zero load-address gate).

Parses the ESP image header DIRECTLY instead of regex-scraping esptool's
human-readable output, and fails if any segment has load address 0.

ESP image layout (ESP32-S3):
    offset 0x00   1 byte   magic                    (0xE9)
    offset 0x01   1 byte   segment count
    offset 0x02   1 byte   flash mode
    offset 0x03   1 byte   flash size + frequency
    offset 0x04   4 bytes  entry point              (LE u32)
    offset 0x08  16 bytes  extended header (wp pin, drive, chip id ...)
    offset 0x18           then, REPEATED once per segment:
                              8 bytes  load_addr (LE u32) + data_len (LE u32)
                              data_len bytes  segment payload
    after all segments     pad to 16 bytes, 1 checksum byte, 32-byte SHA-256

    NOTE: the 8-byte segment headers are INTERLEAVED with the payloads -- each
    header is immediately followed by its own segment's data.  There is no
    contiguous "segment table" at 0x18.  Verified against
    `esptool --chip esp32s3 image_info`, whose "File offs" column is the
    offset of each segment's 8-byte header.

Why load_addr == 0 is fatal here (proved on real hardware 2026-09-17):
    NuttX's OWN secondary loader treats it as harmless padding:
        nuttx/arch/xtensa/src/common/espressif/esp_loader.c:65
        "#define IS_PADD(addr) (addr == 0)"
    but that symbol belongs to NuttX's loader, NOT to the chip ROM.  This
    project builds with CONFIG_ESPRESSIF_SIMPLE_BOOT=y, so there is no
    second-stage bootloader: the ROM itself parses nuttx.bin.  The ESP32-S3
    ROM (ESP-ROM:esp32s3-20210327) does not skip the segment -- it copies
    data_len bytes to address 0 and faults:
        Guru Meditation Error: Core 0 panic'ed (StoreProhibited)
        EXCVADDR: 0x00000000
    then reboots forever on TG0WDT_SYS_RST.  (esptool gets away with these
    segments everywhere else because ESP-IDF always parses its app image with
    a second-stage bootloader, which DOES skip them.)

Exit status: 0 = PASS, 1 = FAIL (bad magic / truncated / zero load addr /
too few segments).  The full segment table is printed on both paths.
"""

from __future__ import annotations

import os
import struct
import sys

IMAGE_MAGIC = 0xE9
HEADER_LEN = 24          # 8-byte common header + 16-byte ESP32-S3 extended header
SEG_HEADER_LEN = 8       # load_addr (LE u32) + data_len (LE u32)
MIN_SEGMENTS = 2         # a bootable image needs >=1 code + >=1 data segment;
               # RAM-only builds (all IRAM+DRAM, no IROM/DROM/hash) carry
               # exactly 2 segments and still boot on the ESP32-S3 ROM.


class ImageError(Exception):
    """Raised when the image cannot be parsed or fails the gate."""


def _hex(value: int) -> str:
    """Format a value as a fixed-width 32-bit hex string."""
    return "0x%08x" % (value & 0xFFFFFFFF)


def classify_region(load_addr: int) -> str:
    """Classify a segment by its load address into the ESP32-S3 memory map.

    esptool's human-readable type tokens ([IROM]/[DROM]/[PADDING]/
    [MEM_INTERNAL,IRAM]/[BYTE_ACCESSIBLE,MEM_INTERNAL,DRAM]) are convenient but
    we do not want the gate to depend on esptool's exact wording (or on esptool
    being installed at all).  The load-address windows below are the chip's
    fixed memory map, so classifying by address is authoritative and
    esptool-independent:
        IRAM   0x40370000-0x403E0000  internal instruction RAM
        DRAM   0x3F000000-0x3FFFFFFF  internal data RAM
        DROM   0x3C000000-0x3CFFFFFF  flash-XIP data
        IROM   0x42000000-0x42FFFFFF  flash-XIP code
        PADDING load_addr == 0        (elf2image MMU-page alignment filler)
    """
    if load_addr == 0:
        return "PADDING"
    if 0x40370000 <= load_addr <= 0x403E0000:
        return "IRAM"
    if 0x3C000000 <= load_addr <= 0x3CFFFFFF:
        return "DROM"
    if 0x42000000 <= load_addr <= 0x42FFFFFF:
        return "IROM"
    if 0x3F000000 <= load_addr <= 0x3FFFFFFF:
        return "DRAM"
    return "OTHER"


def _hash_valid(blob: bytes, end_off: int) -> int:
    """Return 1 if an appended SHA-256 digest is present and matches.

    The appended-digest flag is bit 0 of byte 23 of the extended header.  When
    set, the final 32 bytes are the SHA-256 of everything before them.  A
    --ram-only-header image omits the digest entirely, so this returns 0 -- that
    is expected and is NOT a failure for RAM-only builds (they legitimately have
    no Validation Hash, which is exactly why the gate no longer requires one for
    them).
    """
    if not (blob[23] & 1):
        return 0
    doff = len(blob) - 32
    if doff < 56:
        return 0
    import hashlib
    return 1 if hashlib.sha256(blob[:doff]).digest() == blob[doff:] else 0


def parse_image(blob: bytes) -> dict:
    """Parse an ESP32-S3 firmware image.

    Args:
        blob: The complete image file content.

    Returns:
        Dict with ``segcount``, ``entry`` and ``segments``; each element of
        ``segments`` is a dict with ``index``, ``load_addr``, ``data_len``,
        ``hdr_off`` and ``data_off``.

    Raises:
        ImageError: If the magic byte is wrong or any structure is truncated.
    """
    size = len(blob)
    min_size = HEADER_LEN + SEG_HEADER_LEN
    if size < min_size:
        raise ImageError(
            "file is only %d bytes -- not even a %d-byte image header plus one "
            "%d-byte segment header (%d bytes); this is not a complete ESP "
            "firmware image" % (size, HEADER_LEN, SEG_HEADER_LEN, min_size)
        )

    magic = blob[0]
    if magic != IMAGE_MAGIC:
        raise ImageError(
            "bad image magic: byte 0 is 0x%02x, expected 0x%02x "
            "(esp_image_header_t.magic) -- this is not an ESP firmware image"
            % (magic, IMAGE_MAGIC)
        )

    segcount = blob[1]
    entry = struct.unpack_from("<I", blob, 4)[0]

    if segcount < 1:
        raise ImageError(
            "header declares %d segments -- an image with no segments cannot "
            "boot" % segcount
        )

    segments = []
    off = HEADER_LEN
    for index in range(segcount):
        if off + SEG_HEADER_LEN > size:
            raise ImageError(
                "segment table truncated: segment %d's 8-byte header needs file "
                "offsets %d..%d, but the file is only %d bytes (header declares "
                "%d segments)" % (index, off, off + SEG_HEADER_LEN, size, segcount)
            )
        load_addr, data_len = struct.unpack_from("<II", blob, off)
        data_off = off + SEG_HEADER_LEN
        if data_off + data_len > size:
            raise ImageError(
                "segment %d data truncated: it declares load_addr=%s data_len=%d "
                "(0x%x) with payload at file offset %d, which needs %d bytes, but "
                "the file is only %d bytes"
                % (
                    index,
                    _hex(load_addr),
                    data_len,
                    data_len,
                    data_off,
                    data_off + data_len,
                    size,
                )
            )
        segments.append(
            {
                "index": index,
                "load_addr": load_addr,
                "data_len": data_len,
                "hdr_off": off,
                "data_off": data_off,
            }
        )
        off = data_off + data_len

    return {
        "segcount": segcount,
        "entry": entry,
        "segments": segments,
        "end_off": off,
    }


def _pad_probe(blob: bytes, seg: dict) -> str:
    """Describe the payload of a zero-address segment (forensics only)."""
    payload = blob[seg["data_off"]:seg["data_off"] + seg["data_len"]]
    if not payload:
        return "   payload: (empty)"
    first = " ".join("%02x" % b for b in payload[:16])
    if len(set(payload)) == 1:
        filling = "all %d payload bytes are 0x%02x" % (len(payload), payload[0])
    else:
        filling = "payload has mixed content"
    return "   payload[0:16]: %s | %s" % (first, filling)


def render_table(path: str, blob: bytes, image: dict) -> str:
    """Render the whole segment table as a multi-line string."""
    lines = []
    lines.append("== ESP IMAGE SEGMENT TABLE: %s ==" % path)
    lines.append("   file size    : %d bytes" % len(blob))
    lines.append("   magic        : 0x%02x (ok)" % blob[0])
    lines.append("   segcount     : %d" % image["segcount"])
    lines.append("   entry point  : %s" % _hex(image["entry"]))
    lines.append("   first seg hdr: 0x%x  (%d-byte image header)" % (HEADER_LEN, HEADER_LEN))
    lines.append("   last byte of last segment data: 0x%x" % image["end_off"])
    lines.append("")
    lines.append(
        "   %-4s %-12s %-10s %-10s %-10s %s"
        % ("idx", "load_addr", "data_len", "hdr_off", "data_off", "note")
    )
    for seg in image["segments"]:
        if seg["load_addr"] == 0:
            note = "ZERO LOAD ADDR -- ROM FAULTS HERE"
        else:
            note = "-"
        lines.append(
            "   %-4d %-12s 0x%-8x 0x%-8x 0x%-8x %s"
            % (
                seg["index"],
                _hex(seg["load_addr"]),
                seg["data_len"],
                seg["hdr_off"],
                seg["data_off"],
                note,
            )
        )
    for seg in image["segments"]:
        if seg["load_addr"] == 0:
            lines.append("")
            lines.append(
                "   seg%d load_addr=0x00000000 data_len=0x%x -- this is the segment "
                "the ROM tries to copy to address 0."
                % (seg["index"], seg["data_len"])
            )
            lines.append(_pad_probe(blob, seg))
    return "\n".join(lines)


def check(path: str) -> int:
    """Check one image. Returns 0 on PASS, 1 on FAIL."""
    if not os.path.isfile(path):
        sys.stdout.write("== ZERO-ADDR GATE FAIL: no such file: %s ==\n" % path)
        return 1

    with open(path, "rb") as handle:
        blob = handle.read()

    try:
        image = parse_image(blob)
    except ImageError as exc:
        sys.stdout.write("== ZERO-ADDR GATE FAIL: cannot parse image header ==\n")
        sys.stdout.write("   %s\n" % exc)
        sys.stdout.write("   A corrupt or non-ESP image is never bootable.\n")
        return 1

    sys.stdout.write(render_table(path, blob, image) + "\n")

    zero_segments = [s for s in image["segments"] if s["load_addr"] == 0]
    problems = []
    if zero_segments:
        problems.append(
            "%d segment(s) with load address 0 (indexes: %s)"
            % (
                len(zero_segments),
                ", ".join(str(s["index"]) for s in zero_segments),
            )
        )
    if image["segcount"] < MIN_SEGMENTS:
        problems.append(
            "segment count is %d, minimum for a healthy image is %d"
            % (image["segcount"], MIN_SEGMENTS)
        )

    # One value per line: the CI gates scrape these with an anchored
    # `sed -n 's/^KEY=\([0-9][0-9]*\).*/\1/p'`, so KEY must start the line.
    # ESP32-S3 segment classification by LOAD ADDRESS (esptool-independent):
    #   IRAM 0x40370000-0x403E0000  DRAM 0x3F000000-0x3FFFFFFF
    #   IROM 0x42000000-0x42FFFFFF  DROM 0x3C000000-0x3CFFFFFF
    #   PADDING load_addr == 0
    # The BOOTABILITY GATE uses these to tell a RAM-only build
    # (IRAM+DRAM, no hash) from a flash-XIP build (IROM+DROM+hash).
    counts = {"IRAM": 0, "DRAM": 0, "IROM": 0, "DROM": 0, "PADDING": 0, "OTHER": 0}
    for seg in image["segments"]:
        counts[classify_region(seg["load_addr"])] += 1
    hash_valid = _hash_valid(blob, image["end_off"])
    sys.stdout.write("\nSEGCOUNT=%d\n" % image["segcount"])
    sys.stdout.write("ZERO_LOAD_ADDR_SEGMENTS=%d\n" % len(zero_segments))
    sys.stdout.write("ENTRY=%s\n" % _hex(image["entry"]))
    sys.stdout.write("IRAM_COUNT=%d\n" % counts["IRAM"])
    sys.stdout.write("DRAM_COUNT=%d\n" % counts["DRAM"])
    sys.stdout.write("IROM_COUNT=%d\n" % counts["IROM"])
    sys.stdout.write("DROM_COUNT=%d\n" % counts["DROM"])
    sys.stdout.write("HASH_VALID=%d\n" % hash_valid)

    if problems:
        sys.stdout.write("== ZERO-ADDR GATE FAIL: %s ==\n" % "; ".join(problems))
        sys.stdout.write(
            "   A segment with load address 0 is NOT harmless padding on this\n"
            "   board.  nuttx/arch/xtensa/src/common/espressif/esp_loader.c:65\n"
            "   ('#define IS_PADD(addr) (addr == 0)') is NuttX's own SECONDARY\n"
            "   loader, not the chip ROM.  This image is built with\n"
            "   CONFIG_ESPRESSIF_SIMPLE_BOOT=y, so there is no second-stage\n"
            "   bootloader: ESP-ROM:esp32s3-20210327 parses nuttx.bin itself, it\n"
            "   does NOT skip a zero-address segment, it copies data_len bytes to\n"
            "   address 0 and takes a StoreProhibited fault\n"
            "   (Guru Meditation Error ... StoreProhibited, EXCVADDR:\n"
            "   0x00000000), then reboots forever on TG0WDT_SYS_RST.\n"
            "   run#68/#69 (787632 bytes, md5 71e92552cc0fe0f73a3147a6cca5a8ae)\n"
            "   HAVE [IROM], [DROM] and a VALID Validation Hash and still brick\n"
            "   for exactly this reason, which is why the IROM/DROM/hash gate\n"
            "   alone is not sufficient.  esptool labels these segments\n"
            "   [PADDING] purely because its per-chip table maps\n"
            "   0x00000000-0x00010000 to 'PADDING'\n"
            "   (esptool/targets/esp32s3.py MEMORY_MAP) -- that is esptool's\n"
            "   classification, not a promise from the ROM.\n"
            "   Origin: esptool elf2image MMU-page (64 KB) alignment padding,\n"
            "   esptool/bin_image.py save(): pad_segment = ImageSegment(0, ...).\n"
            "   Full segment table printed above.\n"
        )
        return 1

    sys.stdout.write(
        "ZERO-ADDR GATE OK: segcount=%d, zero-load-address segments=0, entry=%s\n"
        % (image["segcount"], _hex(image["entry"]))
    )
    return 0


def main(argv: list) -> int:
    """Entry point. Returns 0 if every image passes, 1 otherwise."""
    args = [a for a in argv[1:] if a]
    if not args:
        sys.stdout.write("usage: segcheck.py <image.bin> [<image.bin> ...]\n")
        return 2
    rc = 0
    for path in args:
        sys.stdout.write("-" * 78 + "\n")
        if check(path) != 0:
            rc = 1
    sys.stdout.write("-" * 78 + "\n")
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
