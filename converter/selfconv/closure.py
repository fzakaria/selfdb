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


ELF_MAGIC = b"\x7fELF"


def _is_elf(path: str) -> bool:
    """Wrapper scripts share a bin/ directory with real binaries and have no
    closure; skip them rather than failing the whole pack."""
    try:
        with open(path, "rb") as handle:
            return handle.read(4) == ELF_MAGIC
    except OSError:
        return False


def _ldd_map(binary: str) -> dict[str, str]:
    """soname -> resolved path, *as this object sees it*.

    Resolution has to be per-object. Two roots in one database can each need
    `libc.so.6` and mean different store paths, and a single soname->path
    dict across the pack silently gives one of them the other's libc -- the
    `LIMIT 1` bug this table exists to remove. ldd already applied that
    object's own RUNPATH, so keep its answer with the edge.
    """
    resolved = {}
    out = subprocess.run(["ldd", binary], capture_output=True, text=True).stdout
    for line in out.splitlines():
        # "libfoo.so => /nix/store/.../libfoo.so (0x...)"
        if "=>" not in line:
            continue
        soname, rhs = line.split("=>", 1)
        path = rhs.split(" (", 1)[0].strip()
        if path and os.path.exists(path):
            resolved[soname.strip()] = os.path.realpath(path)
    return resolved


def _ldd_closure(binary: str) -> list[str]:
    """Return the resolved store paths of every NEEDED library (transitive)."""
    return sorted(set(_ldd_map(binary).values()))


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


def _insert_object(con, lief, path, is_root, with_segments, seen):
    """Insert one object, or adopt the row a previous root already created.

    `objects.path` is UNIQUE, so a library needed by fifty roots is stored
    once and the sharing falls out of the schema rather than a dedup pass.
    A path can also be reached as a dependency first and named as a root
    later, so is_root is promoted rather than overwritten.
    """
    if path in seen:
        if is_root:
            con.execute("UPDATE objects SET kind='exe', is_root=1 WHERE path=?",
                        (path,))
        return seen[path]

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

    resolved = _ldd_map(path)
    for ord_, lib in enumerate(b.libraries):
        con.execute(
            "INSERT INTO needs (object_id, ord, soname, resolved_path)"
            " VALUES (?,?,?,?)",
            (oid, ord_, lib, resolved.get(lib)))

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
    seen[path] = oid
    return oid


def build_closure(roots, out: str, with_segments: bool = True) -> dict:
    """Pack one or more roots and their closures into a single database.

    Several roots in one file is the same schema, not a different one: the
    executables are simply more rows in `objects`, and any library they have
    in common is one row that both point at.
    """
    import lief

    if isinstance(roots, str):
        roots = [roots]
    wanted = [os.path.realpath(r) for r in roots]

    if os.path.exists(out):
        os.remove(out)
    con = sqlite3.connect(out)
    con.execute("PRAGMA page_size = 4096")
    con.executescript(CLOSURE_SCHEMA)
    con.execute(f"PRAGMA application_id = {APPLICATION_ID}")
    con.execute(f"PRAGMA user_version = {FORMAT_VERSION}")

    seen: dict[str, int] = {}
    skipped = []
    for root in wanted:
        if not _is_elf(root):
            skipped.append(root)
            continue
        _insert_object(con, lief, root, True, with_segments, seen)
        for lib in _ldd_closure(root):
            _insert_object(con, lief, lib, False, with_segments, seen)

    con.commit()
    con.execute("VACUUM")
    stats = {
        "objects": con.execute("SELECT count(*) FROM objects").fetchone()[0],
        "roots": con.execute(
            "SELECT count(*) FROM objects WHERE is_root=1").fetchone()[0],
        "edges": con.execute("SELECT count(*) FROM needs").fetchone()[0],
        "skipped": skipped,
    }
    con.close()
    return stats


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Pack one or more binaries and their dependency closures "
                    "into one SQLite database (resolution as a foreign key).")
    # The one-root form stays exactly as it was: `self closure BIN [OUT]`.
    ap.add_argument("binary", nargs="?", help="a root; repeat with --root")
    ap.add_argument("out", nargs="?", help="default: <binary>.closure.db")
    ap.add_argument("--root", action="append", default=[], metavar="PATH",
                    help="an additional root; may be given more than once")
    ap.add_argument("--roots-from", metavar="FILE",
                    help="read roots from FILE, one per line ('-' for stdin)")
    ap.add_argument("-o", "--out", dest="out_flag", metavar="DB",
                    help="output database; required when no positional root")
    ap.add_argument("--no-segments", action="store_true",
                    help="metadata only (graph + symbols, no segment bytes)")
    args = ap.parse_args(argv)

    if not shutil.which("ldd"):
        print("self closure: needs ldd on PATH", file=sys.stderr)
        return 1

    roots = ([args.binary] if args.binary else []) + args.root
    if args.roots_from:
        stream = sys.stdin if args.roots_from == "-" else open(args.roots_from)
        with stream:
            roots += [line.strip() for line in stream if line.strip()]
    if not roots:
        ap.error("give a root as an argument, with --root, or via --roots-from")

    # The second positional is the output only in the one-root form; with
    # --root or --roots-from there is nothing to distinguish it from a root,
    # so those spellings ask for -o instead.
    out = args.out_flag or args.out
    if out is None:
        if len(roots) > 1:
            ap.error("several roots need an explicit -o/--out")
        out = roots[0] + ".closure.db"

    stats = build_closure(roots, out, with_segments=not args.no_segments)
    for path in stats["skipped"]:
        print(f"self closure: skipping {path}: not an ELF file", file=sys.stderr)
    print(f"{stats['roots']} root(s) + closure -> {out} "
          f"({stats['objects']} objects, {stats['edges']} edges)",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
