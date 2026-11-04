import json
import os
import struct
import tempfile
import unittest

import wolf_survey as ws


def s(text):
    raw = text.encode("utf-8") + b"\0"
    return struct.pack("<I", len(raw)) + raw


def cmd(code, args=(), strings=(), indent=0, route=None):
    out = bytes([len(args) + 1]) + struct.pack("<I", code)
    out += b"".join(struct.pack("<i", a) for a in args)
    out += bytes([indent, len(strings)]) + b"".join(s(x) for x in strings)
    if route is None:
        return out + b"\0"
    out += b"\1" + b"\0" * 5 + b"\0" + struct.pack("<I", len(route))
    for rid, rargs in route:
        out += bytes([rid, len(rargs)]) + b"".join(struct.pack("<I", a) for a in rargs) + b"\1\0"
    return out


def cmd_list(*cmds):
    return struct.pack("<I", len(cmds)) + b"".join(cmds)


def pe_with_version(ms, ls):
    """A minimal PE32 whose resource directory holds a VS_FIXEDFILEINFO."""
    pe_off = 0x40
    opt_size = 96 + 16 * 8
    sec_hdr = pe_off + 24 + opt_size
    raw = 0x200
    res = b"\0" * 16 + struct.pack("<IIIII", 0xFEEF04BD, 0x10000, ms, ls, 0) + b"\0" * 32
    b = bytearray(raw + len(res))
    b[0:2] = b"MZ"
    struct.pack_into("<I", b, 0x3C, pe_off)
    b[pe_off:pe_off + 4] = b"PE\0\0"
    struct.pack_into("<HH", b, pe_off + 4, 0x14C, 1)
    struct.pack_into("<H", b, pe_off + 20, opt_size)
    opt = pe_off + 24
    struct.pack_into("<H", b, opt, 0x10B)
    struct.pack_into("<II", b, opt + 96 + 2 * 8, 0x1000, len(res))  # resource directory
    b[sec_hdr:sec_hdr + 8] = b".rsrc\0\0\0"
    struct.pack_into("<IIII", b, sec_hdr + 8, len(res), 0x1000, len(res), raw)
    b[raw:] = res
    return bytes(b)


class CommandScanner(unittest.TestCase):
    def test_counts_a_list_with_strings_and_a_route(self):
        body = cmd_list(cmd(101, (), ["Hello"]),
                        cmd(121, (1000000, 5, 0)),
                        cmd(201, (-1,), route=[(3, ()), (24, (60,))]),
                        cmd(0))
        noise = bytes(range(256)) * 4
        c, n = ws.count_commands(noise + body + noise)
        self.assertEqual(n, 1)
        self.assertEqual(c, {101: 1, 121: 1, 201: 1, 0: 1})

    def test_finds_several_lists(self):
        a = cmd_list(cmd(101, (), ["a"]), cmd(0))
        b = cmd_list(cmd(150, (1, 2, 3), ["pic.png"]), cmd(150, (1, 2, 3)), cmd(0))
        c, n = ws.count_commands(b"\x8e\0\0\0" + a + b"\x8f" + b + b"\x90")
        self.assertEqual(n, 2)
        self.assertEqual(c[150], 2)

    def test_strict_needs_the_blank_end_and_loose_does_not(self):
        body = cmd_list(cmd(101, (), ["x"]), cmd(102, (2,), ["yes", "no"]))
        self.assertEqual(ws.count_commands(body)[1], 0)
        self.assertEqual(ws.count_commands(body, strict=False)[1], 1)

    def test_rejects_absurd_lengths_without_reading_past_the_end(self):
        bad = struct.pack("<I", 1) + bytes([2]) + struct.pack("<I", 101) + struct.pack("<i", 0) \
            + bytes([0, 1]) + struct.pack("<I", 0xFFFFFFF0)
        self.assertEqual(ws.count_commands(bad)[1], 0)
        self.assertEqual(ws.count_commands(b"")[1], 0)


class PeVersion(unittest.TestCase):
    def test_reads_the_fixed_file_version(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "Game.exe")
            with open(p, "wb") as f:
                f.write(pe_with_version((2 << 16) | 281, (0 << 16) | 0))
            self.assertEqual(ws.pe_file_version(p), "2.281.0.0")

    def test_not_a_pe(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "Game.exe")
            with open(p, "wb") as f:
                f.write(b"MZ" + b"\0" * 100)
            self.assertIsNone(ws.pe_file_version(p))


class Survey(unittest.TestCase):
    def make_game(self, top, name, pro=False, packed=False):
        g = os.path.join(top, name)
        os.makedirs(os.path.join(g, "Data", "BasicData"))
        os.makedirs(os.path.join(g, "Data", "MapData"))
        with open(os.path.join(g, "GamePro.exe" if pro else "Game.exe"), "wb") as f:
            f.write(pe_with_version((3 << 16) | 50, 0))
        if packed:
            with open(os.path.join(g, "Data", "BasicData.wolf"), "wb") as f:
                f.write(b"\x11\x22" * 32)
            return g
        ce = b"\0W\0\0OL\x55FC\0\x01" + struct.pack("<I", 1) + b"\x8e" + struct.pack("<I", 0) + b"\0" * 7 \
            + s("ev") + cmd_list(cmd(101, (), ["hi"]), cmd(0)) + b"\x8f"
        with open(os.path.join(g, "Data", "BasicData", "CommonEvent.dat"), "wb") as f:
            f.write(ce)
        with open(os.path.join(g, "Data", "MapData", "Map000.mps"), "wb") as f:
            f.write(b"\0" * 40 + cmd_list(cmd(210, (500001,)), cmd(101, (), ["map"]), cmd(0)) + b"\x66")
        return g

    def test_survey_reports_versions_pro_and_commands(self):
        with tempfile.TemporaryDirectory() as top:
            self.make_game(top, "plain")
            self.make_game(top, "pro", pro=True, packed=True)
            out = os.path.join(top, "report.json")
            self.assertEqual(ws.main([top, "--out", out]), 0)
            with open(out, encoding="utf-8") as f:
                report = json.load(f)
            summary = report["summary"]
            self.assertEqual(summary["games"], 2)
            self.assertEqual(summary["refused"]["pro"], 1)
            self.assertEqual(summary["engineVersions"], {"3.50.0.0": 2})
            uses = {c["code"]: c["uses"] for c in summary["commandsByUse"]}
            self.assertEqual(uses, {101: 2, 0: 2, 210: 1})
            plain = next(g for g in report["games"] if g["path"].endswith("plain"))
            self.assertEqual(plain["encoding"], "utf-8")
            self.assertTrue(plain["unpacked"])
            packed = next(g for g in report["games"] if g["path"].endswith("pro"))
            self.assertEqual(packed["archives"][0]["head"], "1122" * 8)

    def test_survey_does_not_write_into_the_games(self):
        with tempfile.TemporaryDirectory() as top, tempfile.TemporaryDirectory() as outdir:
            g = self.make_game(top, "plain")
            before = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(g) for f in fs)
            ws.main([top, "--out", os.path.join(outdir, "r.json")])
            after = sorted(os.path.join(dp, f) for dp, _, fs in os.walk(g) for f in fs)
            self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()
