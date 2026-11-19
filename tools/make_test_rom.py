#!/usr/bin/env python3
"""Builds tiny synthetic .nds ROMs for testing the ndsmusic loader (no real game data).

usage: make_test_rom.py <out.nds> [--sdat-name NAME] [--no-symb] [--corrupt]
"""
import argparse
import struct


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def fake_file(magic, n=32):
    return magic + struct.pack("<I", n) + bytes(range(n - 8))


def build_sdat(with_symb=True):
    # FILE contents, indexed by SDAT file id
    files = [
        fake_file(b"SSEQ", 48),   # 0 seq 0
        fake_file(b"SSEQ", 64),   # 1 seq 2
        fake_file(b"SBNK", 40),   # 2 bank 0
        fake_file(b"SWAR", 80),   # 3 wavearc 0
        fake_file(b"SWAR", 96),   # 4 wavearc 1
        fake_file(b"STRM", 128),  # 5 strm 0
    ]

    def record_block(magic, records, entry_bytes):
        """records: list (8 types) of lists of entries (bytes or None)."""
        hdr_len = 8 + 8 * 4 + 24  # magic,size,8 offsets,reserved
        body = bytearray()
        rec_offs = []
        # first lay out entry payloads, then tables
        payload = bytearray()
        payload_offs = []
        for rec in records:
            offs = []
            for e in rec:
                if e is None:
                    offs.append(None)
                else:
                    offs.append(len(payload))
                    payload += entry_bytes(e)
            payload_offs.append(offs)
        tables = bytearray()
        table_pos = []
        for offs in payload_offs:
            table_pos.append(len(tables))
            tables += struct.pack("<I", len(offs)) + b"\0" * (4 * len(offs))
        tables_base = hdr_len
        payload_base = hdr_len + len(tables)
        for ti, offs in enumerate(payload_offs):
            for i, o in enumerate(offs):
                v = 0 if o is None else payload_base + o
                struct.pack_into("<I", tables, table_pos[ti] + 4 + 4 * i, v)
            rec_offs.append(tables_base + table_pos[ti])
        body = bytearray(magic + b"\0\0\0\0")
        body += struct.pack("<8I", *rec_offs) + b"\0" * 24
        body += tables + payload
        body = bytearray(pad4(bytes(body)))
        struct.pack_into("<I", body, 4, len(body))
        return bytes(body)

    seq = lambda fid, bank, vol, ply: struct.pack("<HHHBBBBH", fid, 0, bank, vol, 64, 64, ply, 0)
    info_records = [
        [seq(0, 0, 100, 0), None, seq(1, 0, 127, 0)],                       # SEQ (slot 1 empty)
        [],                                                                 # SEQARC
        [struct.pack("<HH4H", 2, 0, 0, 1, 0xFFFF, 0xFFFF)],                 # BANK -> wavearc 0,1
        [struct.pack("<HH", 3, 0), struct.pack("<HH", 4, 0)],               # WAVEARC
        [struct.pack("<BBHI", 1, 0, 0xFFFF, 0)],                            # PLAYER
        [], [],                                                             # GROUP, PLAYER2
        [struct.pack("<HHBBB5x", 5, 0, 100, 64, 0)],                        # STRM
    ]
    info = record_block(b"INFO", info_records, lambda e: e)

    symb = b""
    if with_symb:
        names = [[b"SEQ_BGM_TITLE", None, b"SEQ_BGM_TOWN"], [], [b"BANK_BGM"], [b"WAVE_0", b"WAVE_1"],
                 [b"PLAYER_BGM"], [], [], [b"STRM_INTRO"]]
        symb = record_block(b"SYMB", names, lambda s: s + b"\0")

    hdr_size = 0x40
    symb_off = hdr_size if symb else 0
    info_off = hdr_size + len(symb)
    fat_off = info_off + len(info)
    fat_size = 12 + 16 * len(files)
    file_off = fat_off + fat_size
    file_blk = bytearray(b"FILE\0\0\0\0" + struct.pack("<I", len(files)) + b"\0" * 4)
    fat = bytearray(b"FAT \0\0\0\0" + struct.pack("<I", len(files)))
    for f in files:
        abs_off = file_off + len(file_blk)
        fat += struct.pack("<II8x", abs_off, len(f))
        file_blk += pad4(f)
    struct.pack_into("<I", fat, 4, len(fat))
    struct.pack_into("<I", file_blk, 4, len(file_blk))

    total = file_off + len(file_blk)
    h = bytearray(b"SDAT" + struct.pack("<HHIHH", 0xFEFF, 0x0100, total, hdr_size, 4 if symb else 3))
    h += struct.pack("<8I", symb_off, len(symb), info_off, len(info), fat_off, len(fat), file_off, len(file_blk))
    h += b"\0" * (hdr_size - len(h))
    return bytes(h + symb + info + fat + file_blk)


def build_rom(sdat, sdat_name, banner=None):
    # Tree: /readme.txt, /data/foo.bin, /data/sound/<sdat_name>
    # File ids: 0 readme, 1 foo, 2 sdat
    contents = [b"hello\n", b"\xAA" * 100, sdat]

    def entry(name, is_dir=False, sub=None):
        n = name.encode()
        b = bytes([len(n) | (0x80 if is_dir else 0)]) + n
        if is_dir:
            b += struct.pack("<H", 0xF000 | sub)
        return b

    sub_root = entry("readme.txt") + entry("data", True, 1) + b"\0"
    sub_data = entry("foo.bin") + entry("sound", True, 2) + b"\0"
    sub_sound = entry(sdat_name) + b"\0"
    main_len = 3 * 8
    o0 = main_len
    o1 = o0 + len(sub_root)
    o2 = o1 + len(sub_data)
    fnt = struct.pack("<IHH", o0, 0, 3) + struct.pack("<IHH", o1, 1, 0xF000) + struct.pack("<IHH", o2, 2, 0xF001)
    fnt = pad4(fnt + sub_root + sub_data + sub_sound)

    rom = bytearray(0x200)
    rom[0:12] = b"TESTSOUNDROM"
    rom[12:16] = b"TEST"
    fnt_off = len(rom)
    rom += fnt
    fat_off = len(rom)
    fat_size = 8 * len(contents)
    rom += b"\0" * fat_size
    for i, c in enumerate(contents):
        while len(rom) % 0x200:
            rom += b"\xFF"
        start = len(rom)
        rom += c
        struct.pack_into("<II", rom, fat_off + 8 * i, start, start + len(c))
    struct.pack_into("<IIII", rom, 0x40, fnt_off, len(fnt), fat_off, fat_size)
    if banner:
        while len(rom) % 0x200:
            rom += b"\xFF"
        struct.pack_into("<I", rom, 0x68, len(rom))
        rom += banner
    return bytes(rom)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--sdat-name", default="sound_data.sdat")
    ap.add_argument("--no-symb", action="store_true")
    ap.add_argument("--corrupt", action="store_true", help="point root dir count past FNT end")
    a = ap.parse_args()
    rom = bytearray(build_rom(build_sdat(not a.no_symb), a.sdat_name))
    if a.corrupt:
        fnt_off = struct.unpack_from("<I", rom, 0x40)[0]
        struct.pack_into("<H", rom, fnt_off + 6, 0x7FFF)
    open(a.out, "wb").write(rom)
