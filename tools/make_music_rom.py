#!/usr/bin/env python3
"""Builds a small *playable* synthetic SDAT (no real game data) for testing the player:
PCM16 / PCM8 / IMA-ADPCM samples, PSG + noise, drum set + key split instruments,
two tracks with loop, call/return, plus a stereo looping STRM.

usage: make_music_rom.py out.nds [--ncsf out_dir]
  --ncsf also writes test.ncsflib + test.minincsf (SDAT in the lib, seq number in the mini).
"""
import argparse
import math
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_rom import build_rom, pad4  # noqa: E402

CLK = 16756991

# ---------------------------------------------------------------- ADPCM (DS flavour)
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
        97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
        796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
        4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]
IDX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_step(pred, idx, n):
    step = STEP[idx]
    diff = step >> 3
    if n & 1: diff += step >> 2
    if n & 2: diff += step >> 1
    if n & 4: diff += step
    pred = max(-0x7FFF, pred - diff) if n & 8 else min(0x7FFF, pred + diff)
    idx = min(88, max(0, idx + IDX[n & 7]))
    return pred, idx


def adpcm_encode(samples):
    pred, idx = samples[0], 0
    hdr = struct.pack("<hBB", pred, idx, 0)
    nibs = []
    for x in samples:
        d = x - pred
        n = 0
        if d < 0:
            n, d = 8, -d
        step = STEP[idx]
        if d >= step: n |= 4; d -= step
        step >>= 1
        if d >= step: n |= 2; d -= step
        step >>= 1
        if d >= step: n |= 1
        pred, idx = adpcm_step(pred, idx, n)
        nibs.append(n)
    if len(nibs) % 2: nibs.append(0)
    return hdr + bytes(nibs[i] | (nibs[i + 1] << 4) for i in range(0, len(nibs), 2))


# ---------------------------------------------------------------- file builders
def nitro(magic, blocks):
    body = b"".join(blocks)
    return magic + struct.pack("<HHIHH", 0xFEFF, 0x0100, 16 + len(body), 16, len(blocks)) + body


def swav_entry(fmt, loop, rate, data, loop_samples):
    timer = round(CLK / rate)
    bps = {0: 1, 1: 2}.get(fmt)
    data = pad4(data)
    if fmt == 2:
        loop_words = 0
    else:
        loop_words = loop_samples * bps // 4
    total_words = len(data) // 4
    return struct.pack("<BBHHHI", fmt, loop, rate, timer, loop_words, total_words - loop_words) + data


def build_swar(entries):
    hdr_len = 0x3C + 4 * len(entries)
    offs, body, pos = [], b"", 0x10 + hdr_len - 0x10
    data_start = 0x3C + 4 * len(entries)
    pos = data_start
    for e in entries:
        offs.append(pos)
        body += e
        pos += len(e)
    data = b"DATA" + struct.pack("<I", 8 + 32 + 4 + 4 * len(entries) + len(body)) + b"\0" * 32
    data += struct.pack("<I", len(entries)) + b"".join(struct.pack("<I", o) for o in offs) + body
    return nitro(b"SWAR", [data])


def build_sbnk(instruments):
    """instruments: list of (type, payload_bytes)."""
    n = len(instruments)
    base = 0x3C + 4 * n
    recs, payload = b"", b""
    for typ, pl in instruments:
        recs += struct.pack("<BHB", typ, base + len(payload), 0)
        payload += pl
    data = b"DATA" + struct.pack("<I", 0) + b"\0" * 32 + struct.pack("<I", n) + recs + payload
    data = data[:4] + struct.pack("<I", len(data)) + data[8:]
    return nitro(b"SBNK", [data])


def notedef(swav, swar, key, a=127, d=127, s=127, r=100, pan=64):
    return struct.pack("<HHBBBBBB", swav, swar, key, a, d, s, r, pan)


def vlq(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    return bytes(reversed(out))


def u24(v):
    return struct.pack("<I", v)[:3]


def build_sseq():
    # Assemble with label fixups (offsets are relative to the data start).
    prog = bytearray()
    labels, fix = {}, []

    def emit(*bs):
        for b in bs:
            prog.extend(b if isinstance(b, (bytes, bytearray)) else bytes([b]))

    def ref(name):
        fix.append((len(prog), name))
        prog.extend(b"\0\0\0")

    def label(name):
        labels[name] = len(prog)

    # track 0
    emit(0xFE, b"\x03\x00")
    emit(0x93, 1); ref("t1")
    emit(0xE1, b"\x78\x00")       # tempo 120
    emit(0xC1, 127)
    emit(0x81, vlq(0))           # program 0: PCM16 sine
    label("loop")
    emit(69, 127, vlq(96))       # A4 for 96 ticks (~1 s)
    emit(81, 127, vlq(96))       # A5
    emit(0x94); ref("loop")
    # track 1
    label("t1")
    emit(0x81, vlq(1))           # PSG square
    emit(0xC0, 0)                # pan hard left
    emit(57, 100, vlq(192))      # A3 for 2 s
    emit(0x95); ref("sub")
    emit(0xFF)
    label("sub")
    emit(0x81, vlq(2))           # drum set
    emit(36, 127, vlq(24))       # ADPCM hit
    emit(37, 127, vlq(24))       # noise
    emit(38, 127, vlq(24))       # PCM8 full-scale square
    emit(0xFD)

    for pos, name in fix:
        prog[pos:pos + 3] = u24(labels[name])
    data = b"DATA" + struct.pack("<II", 12 + len(prog), 0x1C) + bytes(prog)
    return nitro(b"SSEQ", [data])


def build_strm():
    rate, total, bs = 32000, 32000, 1000
    nblocks = (total + bs - 1) // bs
    L = [int(12000 * math.sin(2 * math.pi * 330 * i / rate)) for i in range(total)]
    R = [int(12000 * math.sin(2 * math.pi * 660 * i / rate)) for i in range(total)]
    blen = bs * 2
    last_s = total - (nblocks - 1) * bs
    last_len = last_s * 2
    data = b""
    for b in range(nblocks):
        for ch in (L, R):
            seg = ch[b * bs:(b + 1) * bs]
            data += struct.pack("<%dh" % len(seg), *seg)
    head_size = 0x50
    data_off = 0x10 + head_size + 8
    head = b"HEAD" + struct.pack("<I", head_size) + struct.pack(
        "<BBBBHHIIIIIIII", 1, 1, 2, 0, rate, round(CLK / rate), 0, total, data_off, nblocks, blen, bs, last_len, last_s)
    head += b"\0" * (head_size - len(head))
    return nitro(b"STRM", [head, b"DATA" + struct.pack("<I", 8 + len(data)) + data])


def record_block(magic, records, conv):
    """conv(entry, abs_pos) -> bytes; abs_pos is where the entry lands in the block."""
    hdr_len = 8 + 32 + 24
    pbase0 = hdr_len + sum(4 + 4 * len(r) for r in records)
    tables, payload, tpos, poffs = bytearray(), bytearray(), [], []
    for rec in records:
        offs = []
        for e in rec:
            if e is None:
                offs.append(None)
            else:
                offs.append(len(payload))
                payload += conv(e, pbase0 + len(payload))
        poffs.append(offs)
        tpos.append(len(tables))
        tables += struct.pack("<I", len(rec)) + b"\0" * (4 * len(rec))
    pbase = hdr_len + len(tables)
    for ti, offs in enumerate(poffs):
        for i, o in enumerate(offs):
            struct.pack_into("<I", tables, tpos[ti] + 4 + 4 * i, 0 if o is None else pbase + o)
    blk = bytearray(magic + b"\0\0\0\0" + struct.pack("<8I", *[hdr_len + p for p in tpos]) + b"\0" * 24)
    blk += tables + payload
    blk = bytearray(pad4(bytes(blk)))
    struct.pack_into("<I", blk, 4, len(blk))
    return bytes(blk)


def build_ssar():
    """Sound-effect archive: shared sequence data, entries pointing into it."""
    prog = bytearray()
    labels, fix = {}, []

    def emit(*bs):
        for b in bs:
            prog.extend(b if isinstance(b, (bytes, bytearray)) else bytes([b]))

    def ref(name):
        fix.append((len(prog), name))
        prog.extend(b"\0\0\0")

    def label(name):
        labels[name] = len(prog)

    label("e0"); emit(0x81, vlq(0), 76, 127, vlq(48), 0xFF)          # E5 (659 Hz) one-shot
    label("e2"); emit(0x81, vlq(0), 0x95); ref("sub"); emit(0xFF)     # calls shared subroutine
    label("e3"); emit(0x81, vlq(0)); label("lp"); emit(72, 127, vlq(24), 0x94); ref("lp")  # C5, loops forever
    label("sub"); emit(64, 127, vlq(48), 0xFD)                        # E4 (330 Hz)
    for pos, name in fix:
        prog[pos:pos + 3] = u24(labels[name])

    entries = [labels["e0"], 0xFFFFFFFF, labels["e2"], labels["e3"]]   # slot 1 is empty
    data_off = 0x20 + 12 * len(entries)
    recs = b"".join(struct.pack("<IHBBBBH", o, 0, 127, 64, 64, 0, 0) for o in entries)
    body = b"DATA" + struct.pack("<III", 16 + len(recs) + len(prog), data_off, len(entries)) + recs + bytes(prog)
    return nitro(b"SSAR", [body])


def symb_seqarc(e, pos):
    """SYMB SEQARC entry: {u32 name offset, u32 entry-name table offset} + the strings."""
    name, subs = e
    out = bytearray(8)
    name_at = pos + len(out)
    out += name + b"\0"
    sub_at = pos + len(out)
    tbl = bytearray(struct.pack("<I", len(subs)) + b"\0" * (4 * len(subs)))
    strings = bytearray()
    base = sub_at + len(tbl)
    for i, sn in enumerate(subs):
        if sn is not None:
            struct.pack_into("<I", tbl, 4 + 4 * i, base + len(strings))
            strings += sn + b"\0"
    out += tbl + strings
    struct.pack_into("<II", out, 0, name_at, sub_at)
    return bytes(out)


def build_sdat():
    sine = [int(20000 * math.sin(2 * math.pi * i / 64)) for i in range(64)]
    hit = [int(20000 * math.exp(-i / 800) * math.sin(2 * math.pi * 880 * i / 32000)) for i in range(4000)]
    square = [127 if i < 16 else -128 for i in range(32)]  # full scale: exercises interpolation range
    swar = build_swar([
        swav_entry(1, 1, 28160, struct.pack("<64h", *sine), 0),          # 0: PCM16 sine, 440 Hz @ key 69
        swav_entry(2, 0, 32000, adpcm_encode(hit), 0),                   # 1: ADPCM 880 Hz decaying hit
        swav_entry(0, 1, 14080, bytes(v & 0xFF for v in square), 0),     # 2: PCM8 square, 440 Hz @ key 69
    ])
    drum = bytes([36, 38]) + b"".join(struct.pack("<BB", t, 0) + d for t, d in [
        (1, notedef(1, 0, 60, r=127)), (3, notedef(0, 0, 69, r=127)), (1, notedef(2, 0, 69))])
    split = bytes([60, 127, 0, 0, 0, 0, 0, 0]) + struct.pack("<BB", 1, 0) + notedef(2, 0, 69) + \
        struct.pack("<BB", 1, 0) + notedef(0, 0, 69)
    sbnk = build_sbnk([(1, notedef(0, 0, 69)), (2, notedef(3, 0, 69)), (16, drum), (17, split)])
    files = [build_sseq(), sbnk, swar, build_strm(), build_ssar()]

    seq = lambda fid, bank, vol: struct.pack("<HHHBBBBH", fid, 0, bank, vol, 64, 64, 0, 0)
    info = record_block(b"INFO", [
        [seq(0, 0, 127)], [struct.pack("<HH", 4, 0)], [struct.pack("<HH4H", 1, 0, 0, 0xFFFF, 0xFFFF, 0xFFFF)],
        [struct.pack("<HH", 2, 0)], [struct.pack("<BBHI", 1, 0, 0, 0)], [], [],
        [struct.pack("<HHBBB5x", 3, 0, 127, 64, 0)]], lambda e, pos: e)
    names = [[b"SEQ_TEST_SONG"], [(b"SAR_TEST_SE", [b"SE_PING", None, b"SE_SUB_CALL", b"SE_LOOP"])],
             [b"BANK_TEST"], [b"WAVE_TEST"], [b"PLAYER"], [], [], [b"STRM_TEST"]]
    symb = record_block(b"SYMB", names, lambda e, pos: symb_seqarc(e, pos) if isinstance(e, tuple) else e + b"\0")

    hdr = 0x40
    symb_off, info_off = hdr, hdr + len(symb)
    fat_off = info_off + len(info)
    file_off = fat_off + 12 + 16 * len(files)
    fat = bytearray(b"FAT " + struct.pack("<II", 12 + 16 * len(files), len(files)))
    fblk = bytearray(b"FILE\0\0\0\0" + struct.pack("<I", len(files)) + b"\0" * 4)
    for f in files:
        fat += struct.pack("<II8x", file_off + len(fblk), len(f))
        fblk += pad4(f)
    struct.pack_into("<I", fblk, 4, len(fblk))
    total = file_off + len(fblk)
    h = bytearray(b"SDAT" + struct.pack("<HHIHH", 0xFEFF, 0x0100, total, hdr, 4))
    h += struct.pack("<8I", symb_off, len(symb), info_off, len(info), fat_off, len(fat), file_off, len(fblk))
    h += b"\0" * (hdr - len(h))
    return bytes(h + symb + info + fat + fblk)


# ---------------------------------------------------------------- banner (DS Menu icon)
def icon_tiles(pix):
    """pix: 32x32 list of palette indices (row-major) -> 512-byte tiled 4bpp bitmap."""
    out = bytearray()
    for ty in range(4):
        for tx in range(4):
            for y in range(8):
                for x in range(0, 8, 2):
                    a = pix[(ty * 8 + y) * 32 + tx * 8 + x]
                    b = pix[(ty * 8 + y) * 32 + tx * 8 + x + 1]
                    out.append(a | (b << 4))
    return bytes(out)


def bgr555(r, g, b):
    return (b >> 3) << 10 | (g >> 3) << 5 | (r >> 3)


def test_icon(variant):
    """Distinct, asymmetric patterns so flips and frame order are checkable."""
    pix = [0] * 1024
    for y in range(32):
        for x in range(32):
            if variant == 0: v = 1 + (x // 8)              # vertical bars, colours 1-4
            elif variant == 1: v = 1 + (y // 8)            # horizontal bars
            else: v = 5 if x < 8 and y < 8 else (0 if x > 24 else 6)  # corner block + transparent strip
            pix[y * 32 + x] = v
    return pix


def test_palette(shift):
    cols = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255)]
    cols = cols[:1] + cols[1 + shift:] + cols[1:1 + shift]
    cols += [(128, 128, 128)] * (16 - len(cols))
    return struct.pack("<16H", *[bgr555(*c) for c in cols])


def build_banner(animated):
    titles = ["Test Song ROM\nSynthetic Edition\nNobody", "Test Song ROM\nSynthetic Edition\nNobody",
              "Pok\u00e9mon Test\nÉdition\nPersonne", "Test\nAusgabe\nNiemand", "Test\nEdizione\nNessuno",
              "Prueba\nEdici\u00f3n\nNadie"]
    b = bytearray(0x23C0 if animated else 0x840)
    struct.pack_into("<H", b, 0, 0x0103 if animated else 0x0001)
    b[0x20:0x220] = icon_tiles(test_icon(0))
    b[0x220:0x240] = test_palette(0)
    for i, t in enumerate(titles):
        enc = t.encode("utf-16-le")[:0xFE]
        b[0x240 + i * 0x100:0x240 + i * 0x100 + len(enc)] = enc
    if animated:
        for i in range(3):
            b[0x1240 + i * 0x200:0x1440 + i * 0x200] = icon_tiles(test_icon(i))
        for i in range(2):
            b[0x2240 + i * 0x20:0x2260 + i * 0x20] = test_palette(i)
        seq = [(10, 0, 0, 0, 0), (20, 1, 1, 0, 0), (5, 2, 0, 1, 0), (7, 2, 1, 0, 1)]  # dur, bmp, pal, flipH, flipV
        for i, (d, bm, pl, fh, fv) in enumerate(seq):
            struct.pack_into("<H", b, 0x2340 + i * 2, d | bm << 8 | pl << 11 | fh << 14 | fv << 15)
    return bytes(b)


def psf(reserved, program, tags):
    comp = zlib.compress(program, 9) if program else b""
    out = b"PSF\x25" + struct.pack("<III", len(reserved), len(comp), zlib.crc32(comp)) + reserved + comp
    if tags:
        out += b"[TAG]" + "".join("%s=%s\n" % kv for kv in tags.items()).encode()
    return out


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--ncsf")
    ap.add_argument("--banner", choices=["none", "static", "animated"], default="animated")
    a = ap.parse_args()
    sdat = build_sdat()
    banner = None if a.banner == "none" else build_banner(a.banner == "animated")
    open(a.out, "wb").write(build_rom(sdat, "sound_data.sdat", banner))
    if a.ncsf:
        os.makedirs(a.ncsf, exist_ok=True)
        open(os.path.join(a.ncsf, "test.ncsflib"), "wb").write(psf(b"", sdat, {}))
        open(os.path.join(a.ncsf, "test.minincsf"), "wb").write(psf(struct.pack("<I", 0), b"", {
            "_lib": "test.ncsflib", "title": "Test Song", "game": "Synthetic", "length": "0:03.5", "fade": "1.5"}))
