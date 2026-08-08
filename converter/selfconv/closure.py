"""self closure: pack a binary AND its whole dependency closure into ONE
SQLite database.

This is the "system-wide database, done right" answer to the resolver-DB
problem. On NixOS a bare soname (`libc.so.6`) has *many* providers in
/nix/store, so a global soname->path table is ambiguous. But a *binary's
closure* is exact: ldd/RUNPATH already resolve every NEEDED edge to a
specific store path. We record that resolved path on the edge, so dynamic
linking becomes a foreign-key JOIN with no soname guessing -- one DB per
"tree", exactly the closure Nix already computes.
"""

import argparse
import os
import shutil
import sqlite3
import subprocess
import sys

from . import elfimage
from .elfimage import PF_R, PF_W, PF_X
from .schema import APPLICATION_ID, FORMAT_VERSION

CLOSURE_SCHEMA = """
-- one row per object (executable or shared library) in the closure
CREATE TABLE objects (
  id       INTEGER PRIMARY KEY,
  path     TEXT UNIQUE NOT NULL,   -- resolved store path
  soname   TEXT,                   -- DT_SONAME (NULL for the root exe)
  kind     TEXT NOT NULL,          -- 'exe' | 'lib'
  is_root  INTEGER NOT NULL DEFAULT 0,
  machine  TEXT,
  build_id TEXT,
  et       INTEGER, em INTEGER, entry INTEGER, phoff INTEGER, phnum INTEGER
);

-- the dependency graph. resolved_path is the FK that removes all soname
-- ambiguity: the edge names the exact provider, not just a soname.
CREATE TABLE needs (
  object_id     INTEGER NOT NULL REFERENCES objects(id),
  ord           INTEGER NOT NULL,
  soname        TEXT NOT NULL,
  resolved_path TEXT REFERENCES objects(path)
);

-- segments/symbols namespaced by object_id -- the whole userland-slice as rows
CREATE TABLE segments (
  object_id INTEGER NOT NULL REFERENCES objects(id),
  idx INTEGER NOT NULL, type TEXT, ptype INTEGER,
  offset INTEGER, vaddr INTEGER, filesz INTEGER, memsz INTEGER,
  r INTEGER, w INTEGER, x INTEGER, align INTEGER, content BLOB
);
CREATE TABLE symbols (
  object_id INTEGER NOT NULL REFERENCES objects(id),
  name TEXT NOT NULL, version TEXT, value INTEGER, size INTEGER,
  type TEXT, bind TEXT, defined INTEGER NOT NULL, exported INTEGER NOT NULL
);
CREATE INDEX idx_sym_name ON symbols(name);
CREATE INDEX idx_needs_obj ON needs(object_id);

-- ldd(1), imports/exports as views over the whole closure
CREATE VIEW ldd AS
  SELECT o.path AS object, n.soname, n.resolved_path
  FROM needs n JOIN objects o ON o.id = n.object_id ORDER BY o.path, n.ord;
CREATE VIEW exports AS
  SELECT object_id, name, version FROM symbols WHERE exported = 1;
CREATE VIEW imports AS
  SELECT object_id, name, version FROM symbols WHERE defined = 0;
"""


def _ldd_closure(binary: str) -> list[str]:
    """Return the resolved store paths of every NEEDED library (transitive)."""
    paths = set()
    out = subprocess.run(["ldd", binary], capture_output=True, text=True).stdout
    for line in out.splitlines():
        # "libfoo.so => /nix/store/.../libfoo.so (0x...)"
        if "=>" in line:
            rhs = line.split("=>", 1)[1].strip()
            p = rhs.split(" (", 1)[0].strip()
            if p and os.path.exists(p):
                paths.add(os.path.realpath(p))
    return sorted(paths)


def _soname_of(binary) -> str | None:
    for ent in binary.dynamic_entries:
        if str(ent.tag).endswith("SONAME"):
            return ent.name
    return None


def _build_id(binary) -> str | None:
    for n in binary.notes:
        if "BUILD_ID" in str(n.type).upper():
            return bytes(n.description).hex()
    return None


def _insert_object(con, lief, path, is_root, with_segments):
    data = open(path, "rb").read()
    ehdr, phdrs = elfimage.parse_image(data)
    b = lief.ELF.parse(path)
    soname = _soname_of(b)
    cur = con.execute(
        "INSERT INTO objects (path, soname, kind, is_root, machine, build_id,"
        " et, em, entry, phoff, phnum) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        (path, soname, "exe" if is_root else "lib", 1 if is_root else 0,
         elfimage.EM_NAMES.get(ehdr.em, f"em{ehdr.em}"), _build_id(b),
         ehdr.et, ehdr.em, ehdr.entry, ehdr.phoff, len(phdrs)))
    oid = cur.lastrowid

    for ord_, lib in enumerate(b.libraries):
        con.execute("INSERT INTO needs (object_id, ord, soname) VALUES (?,?,?)",
                    (oid, ord_, lib))

    if with_segments:
        for i, p in enumerate(phdrs):
            content = (sqlite3.Binary(data[p.offset:p.offset + p.filesz])
                       if p.ptype == elfimage.PT_LOAD else None)
            con.execute(
                "INSERT INTO segments (object_id, idx, type, ptype, offset,"
                " vaddr, filesz, memsz, r, w, x, align, content)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (oid, i, p.type_name, p.ptype, p.offset, p.vaddr, p.filesz,
                 p.memsz, 1 if p.flags & PF_R else 0, 1 if p.flags & PF_W else 0,
                 1 if p.flags & PF_X else 0, p.align, content))

    for sym in b.dynamic_symbols:
        if not sym.name:
            continue
        try:
            shndx = int(sym.shndx)
        except (TypeError, ValueError):
            shndx = 0
        defined = 1 if shndx not in (0,) else 0
        con.execute(
            "INSERT INTO symbols (object_id, name, version, value, size, type,"
            " bind, defined, exported) VALUES (?,?,?,?,?,?,?,?,?)",
            (oid, sym.name, None, sym.value, sym.size,
             str(sym.type).rsplit(".", 1)[-1].lower(),
             str(sym.binding).rsplit(".", 1)[-1].lower(), defined,
             1 if (defined and sym.binding.name != "LOCAL") else 0))
    return oid, soname


def build_closure(root: str, out: str, with_segments: bool = True) -> None:
    import lief

    root = os.path.realpath(root)
    members = [root] + _ldd_closure(root)

    if os.path.exists(out):
        os.remove(out)
    con = sqlite3.connect(out)
    con.execute("PRAGMA page_size = 4096")
    con.executescript(CLOSURE_SCHEMA)
    con.execute(f"PRAGMA application_id = {APPLICATION_ID}")
    con.execute(f"PRAGMA user_version = {FORMAT_VERSION}")

    soname_to_path = {}
    for path in members:
        _, soname = _insert_object(con, lief, path, path == root, with_segments)
        if soname:
            soname_to_path[soname] = path

    # resolve every NEEDED edge to a concrete member path -- the FK that
    # dissolves soname ambiguity (each soname resolves within THIS closure)
    for (needer_id, ord_, soname) in con.execute(
            "SELECT object_id, ord, soname FROM needs").fetchall():
        con.execute("UPDATE needs SET resolved_path=? WHERE object_id=?"
                    " AND ord=? AND soname=?",
                    (soname_to_path.get(soname), needer_id, ord_, soname))

    con.commit()
    con.execute("VACUUM")
    con.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Pack a binary and its whole dependency closure into one "
                    "SQLite database (resolution as a foreign key).")
    ap.add_argument("binary")
    ap.add_argument("out", nargs="?", help="default: <binary>.closure.db")
    ap.add_argument("--no-segments", action="store_true",
                    help="metadata only (graph + symbols, no segment bytes)")
    args = ap.parse_args(argv)
    if not shutil.which("ldd"):
        print("self closure: needs ldd on PATH", file=sys.stderr)
        return 1
    out = args.out or args.binary + ".closure.db"
    build_closure(args.binary, out, with_segments=not args.no_segments)
    print(f"{args.binary} + closure -> {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
