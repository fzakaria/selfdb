"""self2elf: re-serialize a SELF database into a runnable ELF.

This is the same row->image algorithm the C loader (self-exec) uses for its
memfd mode; keeping a Python twin makes round-trip testing trivial.
"""

import argparse
import os
import sqlite3
import stat
import sys

from .elfimage import Ehdr, Phdr, serialize_image
from .schema import APPLICATION_ID


def open_self(path: str) -> sqlite3.Connection:
    con = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    app_id = con.execute("PRAGMA application_id").fetchone()[0]
    if app_id != APPLICATION_ID:
        raise ValueError(f"{path}: application_id 0x{app_id:08x} is not 'SELF'")
    return con


def load_image(con: sqlite3.Connection) -> tuple[Ehdr, list[Phdr]]:
    meta = dict(con.execute("SELECT key, value FROM self_meta"))
    ehdr = Ehdr(et=meta["et"], em=meta["em"], entry=meta["entry"],
                phoff=meta["phoff"], eflags=meta["eflags"],
                phentsize=meta["phentsize"], phnum=meta["phnum"],
                osabi=meta["osabi"])
    phdrs = []
    for (ptype, offset, vaddr, filesz, memsz, r, w, x, align,
         content) in con.execute(
            "SELECT ptype, offset, vaddr, filesz, memsz, r, w, x, align,"
            " content FROM segments ORDER BY id"):
        flags = (4 if r else 0) | (2 if w else 0) | (1 if x else 0)
        phdrs.append(Phdr(ptype, flags, offset, vaddr, filesz, memsz, align,
                          content))
    return ehdr, phdrs


def convert(self_path: str, elf_path: str) -> None:
    con = open_self(self_path)
    ehdr, phdrs = load_image(con)
    con.close()
    img = serialize_image(ehdr, phdrs)
    with open(elf_path, "wb") as f:
        f.write(img)
    st = os.stat(elf_path)
    os.chmod(elf_path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Re-serialize a SELF (SQLite) executable into ELF.")
    ap.add_argument("self_file")
    ap.add_argument("out", nargs="?", help="default: <self> minus .self, + .elf")
    args = ap.parse_args(argv)
    out = args.out or (args.self_file.removesuffix(".self") + ".elf")
    convert(args.self_file, out)
    print(f"{args.self_file} -> {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
