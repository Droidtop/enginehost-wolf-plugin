#!/usr/bin/env python3
"""Survey a folder of WOLF RPG Editor games, read in place, for the core's broad pass.

Usage:
    python3 -I wolf_survey.py GAMES_DIR [--out report.json] [--max-depth N]

GAMES_DIR is searched for game folders: a folder holding Game.exe or GamePro.exe next to a
Data/ folder or .wolf archives. Nothing is written except the report, nothing is copied, and
nothing is decrypted or unpacked. For each game the report gives:

- the engine version, from the version resource of Game.exe / GamePro.exe;
- packed or unpacked data, each archive's size and its first 16 bytes. Each engine era uses one
  fixed archive key, so archives of one era share their first bytes; the report groups them;
- Pro (GamePro.exe, or a key archive) and other signs that the core must refuse the game;
- for data stored unpacked: the text encoding marker of BasicData, the screen and tile size where
  Game.dat says so, and how many times each event command code appears in CommonEvent.dat and
  MapData/*.mps.

Event commands are counted from the layout described in docs/behaviour/formats.md: a command
list is a u32 count followed by that many commands, and a command is
    u8 argc+1, u32 code, (argc) x u32, u8 indent, u8 nstr, nstr x string, u8 tail
where a string is a u32 length (including its NUL) and its bytes, and tail is 0, or 1 followed
by a move route (5 bytes, u8 flags, u32 count, count x route step). A route step is
    u8 id, u8 argc, argc x u32, 0x01 0x00.
The scanner looks for count-prefixed lists whose every command parses and whose codes are all
plausible, so it does not depend on the container layout of each engine version.
"""
import argparse
import collections
import json
import os
import struct
import sys

MAX_ARGS = 64          # commands seen in the wild carry far fewer
MAX_STRINGS = 16
MAX_CODE = 1000        # codes are three-digit numbers
MAX_STRING = 1 << 20
MAX_LIST = 200000
MAX_ROUTE = 100000
READ_LIMIT = 64 << 20  # never read more than this from one file

PRO_MARKERS = ("GamePro.exe",)
EXE_NAMES = ("Game.exe", "GamePro.exe")


# ---------------------------------------------------------------- PE version resource

def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def pe_file_version(path):
    """FileVersion from a PE's VS_FIXEDFILEINFO, as 'a.b.c.d', or None. Reads at most 64 MiB."""
    try:
        with open(path, "rb") as f:
            data = f.read(READ_LIMIT)
    except OSError:
        return None
    try:
        if data[:2] != b"MZ":
            return None
        pe = _u32(data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            return None
        nsec = _u16(data, pe + 6)
        optsize = _u16(data, pe + 20)
        opt = pe + 24
        magic = _u16(data, opt)
        ddir = opt + (96 if magic == 0x10B else 112)
        rva, size = _u32(data, ddir + 2 * 8), _u32(data, ddir + 2 * 8 + 4)
        if not rva:
            return None
        sec = opt + optsize
        for i in range(nsec):
            s = sec + 40 * i
            vaddr, vsize, raw = _u32(data, s + 12), _u32(data, s + 8), _u32(data, s + 20)
            rawsize = _u32(data, s + 16)
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                base = raw + (rva - vaddr)
                break
        else:
            return None
        # The fixed-info signature 0xFEEF04BD sits inside the version resource; find it in the
        # resource section rather than walking the directory tree.
        sig = struct.pack("<I", 0xFEEF04BD)
        at = data.find(sig, base, base + size)
        if at < 0:
            return None
        ms, ls = _u32(data, at + 8), _u32(data, at + 12)
        return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"
    except (struct.error, IndexError):
        return None


# ---------------------------------------------------------------- event command scanner

class _Bad(Exception):
    pass


def _cmd(buf, o, end):
    """Parse one command at o; return (code, next offset) or raise _Bad."""
    if o + 5 > end:
        raise _Bad
    argc = buf[o] - 1
    if argc < 0 or argc > MAX_ARGS:
        raise _Bad
    code = _u32(buf, o + 1)
    if code > MAX_CODE:
        raise _Bad
    o += 5 + 4 * argc
    if o + 2 > end:
        raise _Bad
    nstr = buf[o + 1]
    if nstr > MAX_STRINGS:
        raise _Bad
    o += 2
    for _ in range(nstr):
        if o + 4 > end:
            raise _Bad
        n = _u32(buf, o)
        if n > MAX_STRING or o + 4 + n > end:
            raise _Bad
        if n and buf[o + 4 + n - 1] != 0:
            raise _Bad
        o += 4 + n
    if o >= end:
        raise _Bad
    tail = buf[o]
    o += 1
    if tail == 1:
        o += 6
        if o + 4 > end:
            raise _Bad
        steps = _u32(buf, o)
        if steps > MAX_ROUTE:
            raise _Bad
        o += 4
        for _ in range(steps):
            if o + 2 > end:
                raise _Bad
            sargc = buf[o + 1]
            o += 2 + 4 * sargc
            if o + 2 > end or buf[o] != 1 or buf[o + 1] != 0:
                raise _Bad
            o += 2
    elif tail != 0:
        raise _Bad
    return code, o


def _list_at(buf, o, end, strict=True):
    """A command list at o (u32 count + commands): return (codes, next offset) or raise _Bad."""
    if o + 4 > end:
        raise _Bad
    n = _u32(buf, o)
    if n == 0 or n > MAX_LIST:
        raise _Bad
    o += 4
    codes = []
    for _ in range(n):
        code, o = _cmd(buf, o, end)
        codes.append(code)
    # Strict: a list ends with the blank command (0) and holds one command besides it. That is
    # how the editor writes event pages as far as we know [unverified], and it cuts false
    # positives in binary tile data. A file where no strict list is found is rescanned loose.
    if len(codes) < 2 or (strict and codes[-1] != 0):
        raise _Bad
    return codes, o


def count_commands(buf, strict=True):
    """Counter of event command codes in buf, and the number of lists found."""
    counts = collections.Counter()
    lists = 0
    o, end = 0, len(buf)
    while o + 4 <= end:
        try:
            codes, nxt = _list_at(buf, o, end, strict)
        except _Bad:
            o += 1
            continue
        counts.update(codes)
        lists += 1
        o = nxt
    return counts, lists


def count_file(path, game):
    """Count one file's commands into game; strict first, loose (and flagged) if strict finds none."""
    buf = _read(path)
    c, n = count_commands(buf)
    if n == 0 and buf:
        c, n = count_commands(buf, strict=False)
        if n:
            game["looseFiles"].append(os.path.basename(path))
    game["commandLists"] += n
    return buf, c


# ---------------------------------------------------------------- one game

def _read(path):
    with open(path, "rb") as f:
        return f.read(READ_LIMIT)


def _find_ci(folder, name):
    """A child of folder matching name case-insensitively (Windows games mix cases)."""
    try:
        for entry in os.listdir(folder):
            if entry.lower() == name.lower():
                return os.path.join(folder, entry)
    except OSError:
        pass
    return None


def survey_game(root):
    game = {"path": root, "exe": {}, "archives": [], "unpacked": False, "refuse": [],
            "encoding": None, "commands": {}, "commandLists": 0, "looseFiles": [], "errors": []}
    for exe in EXE_NAMES:
        p = _find_ci(root, exe)
        if p:
            game["exe"][exe] = pe_file_version(p)
    if any(_find_ci(root, m) for m in PRO_MARKERS):
        game["refuse"].append("pro")
    data_dir = _find_ci(root, "Data")
    archives = []
    for base in filter(None, [root, data_dir]):
        for entry in sorted(os.listdir(base)):
            if entry.lower().endswith(".wolf"):
                archives.append(os.path.join(base, entry))
    for a in archives:
        try:
            with open(a, "rb") as f:
                head = f.read(16)
            game["archives"].append({"name": os.path.relpath(a, root), "size": os.path.getsize(a),
                                     "head": head.hex()})
        except OSError as e:
            game["errors"].append(f"{a}: {e}")
    counts = collections.Counter()
    basic = _find_ci(data_dir, "BasicData") if data_dir else None
    if basic:
        game["unpacked"] = True
        ce = _find_ci(basic, "CommonEvent.dat")
        if ce:
            try:
                buf, c = count_file(ce, game)
                counts += c
                # Byte 6 is the format marker: 0x55 is UTF-8 (3.x), anything else Shift-JIS (2.x).
                game["encoding"] = "utf-8" if len(buf) > 6 and buf[6] == 0x55 else "shift-jis"
            except OSError as e:
                game["errors"].append(f"{ce}: {e}")
    maps = _find_ci(data_dir, "MapData") if data_dir else None
    if maps:
        for entry in sorted(os.listdir(maps)):
            if entry.lower().endswith(".mps"):
                try:
                    _, c = count_file(os.path.join(maps, entry), game)
                    counts += c
                except OSError as e:
                    game["errors"].append(f"{entry}: {e}")
    game["commands"] = {str(k): v for k, v in sorted(counts.items())}
    return game


def is_game(folder):
    if not any(_find_ci(folder, e) for e in EXE_NAMES):
        return False
    if _find_ci(folder, "Data") or _find_ci(folder, "Data.wolf"):
        return True
    try:
        return any(e.lower().endswith(".wolf") for e in os.listdir(folder))
    except OSError:
        return False


def find_games(top, max_depth):
    found = []
    top_depth = top.rstrip(os.sep).count(os.sep)
    for dirpath, dirnames, _ in os.walk(top):
        if dirpath.count(os.sep) - top_depth >= max_depth:
            dirnames[:] = []
        if is_game(dirpath):
            found.append(dirpath)
            dirnames[:] = []  # a game's own subfolders are not more games
    return found


def summarise(games):
    total = collections.Counter()
    used_by = collections.Counter()
    heads = collections.defaultdict(list)
    versions = collections.Counter()
    for g in games:
        for k, v in g["commands"].items():
            total[k] += v
            used_by[k] += 1
        for a in g["archives"]:
            heads[a["head"][:16]].append(g["path"])
        versions[next(iter(g["exe"].values()), None) or "unknown"] += 1
    return {
        "games": len(games),
        "engineVersions": dict(versions.most_common()),
        "unpacked": sum(1 for g in games if g["unpacked"]),
        "looseCounted": sum(1 for g in games if g["looseFiles"]),
        "refused": {r: sum(1 for g in games if r in g["refuse"]) for r in ("pro",)},
        "archiveHeads": {h: len(set(p)) for h, p in sorted(heads.items(), key=lambda kv: -len(kv[1]))},
        "commandsByUse": [{"code": int(k), "uses": total[k], "games": used_by[k]}
                          for k, _ in total.most_common()],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("games_dir")
    ap.add_argument("--out", default="wolf-survey.json")
    ap.add_argument("--max-depth", type=int, default=4)
    a = ap.parse_args(argv)
    games = []
    for root in find_games(a.games_dir, a.max_depth):
        try:
            games.append(survey_game(root))
        except Exception as e:  # one unreadable game must not end the survey
            games.append({"path": root, "errors": [repr(e)], "commands": {}, "archives": [],
                          "exe": {}, "unpacked": False, "refuse": [], "looseFiles": []})
        print(f"{len(games):4d} {root}", file=sys.stderr)
    report = {"summary": summarise(games), "games": games}
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, ensure_ascii=False)
    s = report["summary"]
    print(f"{s['games']} games, {s['unpacked']} unpacked, {s['refused']['pro']} Pro; "
          f"{len(s['commandsByUse'])} command codes seen; report in {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
