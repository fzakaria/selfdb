"""self: the SELF swiss-army CLI (info / q / scan)."""

import argparse
import os
import sqlite3
import struct
import sys

from .schema import APPLICATION_ID
from .self2elf import open_self

RESOLVER_SCHEMA = """
CREATE TABLE IF NOT EXISTS objects (
  id       INTEGER PRIMARY KEY,
  path     TEXT UNIQUE NOT NULL,
  soname   TEXT,
  machine  TEXT,
  build_id TEXT,
  kind     TEXT NOT NULL          -- 'self' | 'elf'
);
CREATE INDEX IF NOT EXISTS idx_objects_soname ON objects(soname, machine);
"""


def cmd_info(args) -> int:
    con = open_self(args.file)
    meta = dict(con.execute("SELECT key, value FROM self_meta"))
    for key in ("format_version", "type", "machine", "entry", "interp",
                "soname", "build_id", "source"):
        print(f"{key:16} {meta.get(key)}")
    for table in ("segments", "symbols", "relocations", "needed", "sections",
                  "notes"):
        (n,) = con.execute(f"SELECT count(*) FROM {table}").fetchone()
        print(f"{table:16} {n} rows")
    return 0


def cmd_q(args) -> int:
    con = open_self(args.file)
    for row in con.execute(args.sql):
        print("|".join("" if v is None else str(v) for v in row))
    return 0


def _sniff(path: str):
    """Return ('self'|'elf', soname, machine, build_id) or None."""
    with open(path, "rb") as f:
        head = f.read(72)
    if len(head) >= 72 and head[:16] == b"SQLite format 3\0":
        (app_id,) = struct.unpack_from(">I", head, 68)
        if app_id != APPLICATION_ID:
            return None
        con = open_self(path)
        meta = dict(con.execute("SELECT key, value FROM self_meta"))
        con.close()
        return "self", meta.get("soname"), meta.get("machine"), meta.get("build_id")
    if head[:4] == b"\x7fELF":
        import lief
        b = lief.ELF.parse(path)
        if b is None:
            return None
        soname = None
        for ent in b.dynamic_entries:
            if str(ent.tag).endswith("SONAME"):
                soname = ent.name
        from .elfimage import EM_NAMES
        em = int(b.header.machine_type)
        machine = EM_NAMES.get(em, f"em{em}")
        return "elf", soname, machine, None
    return None


def cmd_scan(args) -> int:
    con = sqlite3.connect(args.db)
    con.executescript(RESOLVER_SCHEMA)
    count = 0
    for root in args.paths:
        entries = ([root] if os.path.isfile(root) else
                   [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs])
        for path in entries:
            try:
                info = _sniff(path)
            except Exception:
                continue
            if info is None:
                continue
            kind, soname, machine, build_id = info
            con.execute(
                "INSERT INTO objects (path, soname, machine, build_id, kind)"
                " VALUES (?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET"
                " soname=excluded.soname, machine=excluded.machine,"
                " build_id=excluded.build_id, kind=excluded.kind",
                (os.path.abspath(path), soname, machine, build_id, kind))
            count += 1
    con.commit()
    con.close()
    print(f"indexed {count} objects into {args.db}", file=sys.stderr)
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="self", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info", help="summarize a SELF file")
    p.add_argument("file")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("q", help="run SQL against a SELF file")
    p.add_argument("file")
    p.add_argument("sql")
    p.set_defaults(fn=cmd_q)
    p = sub.add_parser("scan", help="index objects into a resolver database")
    p.add_argument("--db", required=True)
    p.add_argument("paths", nargs="+")
    p.set_defaults(fn=cmd_scan)
    p = sub.add_parser("closure",
                       help="pack a binary + its whole closure into one DB")
    p.add_argument("binary")
    p.add_argument("out", nargs="?")
    p.add_argument("--no-segments", action="store_true")
    p.set_defaults(fn=lambda a: __import__(
        "selfconv.closure", fromlist=["build_closure"]).build_closure(
        a.binary, a.out or a.binary + ".closure.db",
        with_segments=not a.no_segments) or 0)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
