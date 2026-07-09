"""elf2self: convert an ELF object into a SELF database."""

import argparse
import json
import os
import sqlite3
import stat
import sys

from . import elfimage
from .elfimage import PF_R, PF_W, PF_X, PT_INTERP
from .schema import APPLICATION_ID, FORMAT_VERSION, SCHEMA_SQL


def _enum_name(value) -> str:
    """'lief.ELF.Symbol.TYPE.FUNC' / 'TYPE.FUNC' -> 'func'."""
    return str(value).rsplit(".", 1)[-1].lower()


# Raw x86-64 relocation numbers for the loader (M3b); LIEF's enum values are
# LIEF-internal, so recover the real number from the name.
R_X86_64 = {
    "NONE": 0, "64": 1, "PC32": 2, "GOT32": 3, "PLT32": 4, "COPY": 5,
    "GLOB_DAT": 6, "JUMP_SLOT": 7, "RELATIVE": 8, "DTPMOD64": 16,
    "DTPOFF64": 17, "TPOFF64": 18, "TLSDESC": 36, "IRELATIVE": 37,
}


def _reloc_name_and_num(r) -> tuple[str, int]:
    name = str(r.type).rsplit(".", 1)[-1]
    name = name.removeprefix("X86_64_").removeprefix("R_X86_64_")
    num = R_X86_64.get(name, -1)
    return name, num


def _dyn_value(ent):
    for attr in ("name", "runpath", "rpath"):
        if hasattr(ent, attr):
            return getattr(ent, attr)
    if hasattr(ent, "array"):
        arr = list(ent.array)
        if arr:
            return json.dumps(arr)
    return int(ent.value)


def _symbol_version(sym):
    try:
        if not sym.has_version:
            return None
        sv = sym.symbol_version
        aux = sv.symbol_version_auxiliary
        return aux.name if aux else None
    except Exception:
        return None


def _insert_symbols(con, syms, source):
    for sym in syms:
        name = sym.name
        if not name:
            continue
        try:
            shndx = int(sym.shndx)
        except (TypeError, ValueError):
            shndx = None
        defined = 1 if shndx not in (0, None) else 0
        con.execute(
            "INSERT INTO symbols (name, version, value, size, type, bind,"
            " visibility, shndx, defined, exported, source)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
            (name, _symbol_version(sym), sym.value, sym.size,
             _enum_name(sym.type), _enum_name(sym.binding),
             _enum_name(sym.visibility), shndx, defined,
             1 if (defined and sym.binding.name != "LOCAL") else 0, source))


def convert(elf_path: str, self_path: str, with_sections: bool = True) -> None:
    import lief  # heavyweight import: keep it inside the entry point

    data = open(elf_path, "rb").read()
    ehdr, phdrs = elfimage.parse_image(data)
    binary = lief.ELF.parse(elf_path)
    if binary is None:
        raise ValueError(f"LIEF failed to parse {elf_path}")

    if os.path.exists(self_path):
        os.remove(self_path)
    con = sqlite3.connect(self_path)
    con.execute("PRAGMA page_size = 4096")
    con.executescript(SCHEMA_SQL)
    con.execute(f"PRAGMA application_id = {APPLICATION_ID}")
    con.execute(f"PRAGMA user_version = {FORMAT_VERSION}")

    # ── segments (the load-bearing part; straight from the raw image) ──
    interp = None
    for i, p in enumerate(phdrs):
        if p.ptype == PT_INTERP:
            interp = data[p.offset:p.offset + p.filesz].rstrip(b"\0").decode()
        con.execute(
            "INSERT INTO segments (id, type, ptype, offset, vaddr, filesz,"
            " memsz, r, w, x, align, content) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
            (i, p.type_name, p.ptype, p.offset, p.vaddr, p.filesz, p.memsz,
             1 if p.flags & PF_R else 0, 1 if p.flags & PF_W else 0,
             1 if p.flags & PF_X else 0, p.align,
             sqlite3.Binary(p.content) if p.content is not None else None))

    # ── dynamic linking tables (via LIEF) ──────────────────────────────
    soname = None
    for ord_, lib in enumerate(binary.libraries):
        con.execute("INSERT INTO needed (ord, soname) VALUES (?,?)", (ord_, lib))
    for ent in binary.dynamic_entries:
        tag = str(ent.tag).rsplit(".", 1)[-1]
        if tag == "NEEDED":
            continue
        value = _dyn_value(ent)
        if tag == "SONAME":
            soname = value
        con.execute("INSERT INTO dynamic_entries (tag, value) VALUES (?,?)",
                    (tag, value))

    _insert_symbols(con, binary.dynamic_symbols, "dynsym")
    _insert_symbols(con, getattr(binary, "symtab_symbols",
                                 getattr(binary, "static_symbols", [])), "symtab")

    sym_ids = {row[1]: row[0] for row in
               con.execute("SELECT id, name FROM symbols WHERE source='dynsym'")}
    for r in binary.relocations:
        name, num = _reloc_name_and_num(r)
        sym_id = sym_ids.get(r.symbol.name) if r.has_symbol and r.symbol.name else None
        con.execute(
            "INSERT INTO relocations (offset, type, rtype, symbol, addend)"
            " VALUES (?,?,?,?,?)",
            (r.address, name, num, sym_id, r.addend))

    # ── optional layers ────────────────────────────────────────────────
    if with_sections:
        SHT_NOBITS = 8
        for i, s in enumerate(binary.sections):
            raw = None
            try:
                stype = int(s.type)
            except (TypeError, ValueError):
                stype = None
            if stype != SHT_NOBITS and s.size:
                raw = sqlite3.Binary(bytes(s.content))
            con.execute(
                "INSERT INTO sections (id, name, type, stype, flags, vaddr,"
                " offset, size, entsize, link, info, align, content)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (i, s.name, _enum_name(s.type), stype, int(s.flags), s.virtual_address,
                 s.offset, s.size, s.entry_size, s.link, s.information,
                 s.alignment, raw))

    build_id = None
    for n in binary.notes:
        kind = str(n.type).rsplit(".", 1)[-1]
        desc = bytes(n.description)
        con.execute("INSERT INTO notes (kind, name, content) VALUES (?,?,?)",
                    (kind, n.name, sqlite3.Binary(desc)))
        if "BUILD_ID" in kind.upper():
            build_id = desc.hex()

    # ── identity ───────────────────────────────────────────────────────
    meta = {
        "format_version": FORMAT_VERSION,
        "type": elfimage.ET_NAMES.get(ehdr.et, str(ehdr.et)), "et": ehdr.et,
        "machine": elfimage.EM_NAMES.get(ehdr.em, f"em{ehdr.em}"), "em": ehdr.em,
        "class": 64, "byte_order": "little", "osabi": ehdr.osabi,
        "eflags": ehdr.eflags, "entry": ehdr.entry, "phoff": ehdr.phoff,
        "phnum": len(phdrs), "phentsize": elfimage.PHDR_SIZE,
        "interp": interp, "soname": soname, "build_id": build_id,
        "page_size": 4096, "source": os.path.abspath(elf_path),
        "created_by": "elf2self 0.1",
    }
    con.executemany("INSERT INTO self_meta (key, value) VALUES (?,?)",
                    meta.items())
    con.execute(
        "INSERT INTO docs (topic, body) VALUES (?,?)",
        ("format",
         "This executable is a SQLite database (SELF format v%d). "
         "Explore it: `.tables`, `.schema`, SELECT * FROM ldd; "
         "SELECT name FROM exports;. It runs via a binfmt_misc interpreter "
         "that maps the rows in `segments`. See https://github.com/fzakaria/selfdb"
         % FORMAT_VERSION))

    con.commit()
    con.execute("VACUUM")
    con.close()

    if os.access(elf_path, os.X_OK):
        st = os.stat(self_path)
        os.chmod(self_path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Convert an ELF object into a SELF (SQLite) executable.")
    ap.add_argument("elf")
    ap.add_argument("out", nargs="?", help="default: <elf>.self")
    ap.add_argument("--no-sections", action="store_true",
                    help="omit the optional sections table (pre-stripped)")
    args = ap.parse_args(argv)
    out = args.out or args.elf + ".self"
    convert(args.elf, out, with_sections=not args.no_sections)
    print(f"{args.elf} -> {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
