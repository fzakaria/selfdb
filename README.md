# selfdb

**SELF** — the *Structured Executable & Linkable Format*: a program that is a
SQLite database instead of an ELF file, and the machinery to actually run it
on Linux/NixOS.

[sqlelf](https://github.com/fzakaria/sqlelf) ([arXiv:2405.03883](https://arxiv.org/abs/2405.03883))
put a SQL view *over* ELF. This project inverts it: the rows *are* the format,
a `binfmt_misc` interpreter executes them, and nixpkgs/NixOS is the vehicle to
run a real slice of a system on it.

```console
$ file hello
hello: SQLite 3.x database, application id 0x53454c46, user version 1
$ ./hello
Hello, world!
$ sqlite3 hello 'SELECT soname FROM ldd'          # ldd, as a query
$ sqlite3 hello 'DELETE FROM sections; VACUUM'    # strip, as a transaction
$ ./hello                                         # still runs
Hello, world!
```

## What's here

- `converter/` — `elf2self`, `self2elf`, and the `self` CLI (Python + LIEF).
- `loader/` — `self-exec`, the binfmt interpreter, with three modes:
  `memfd` (rebuild ELF → `execveat`), `native` (map segments + hand off to
  ld.so), `selfld` (be the dynamic linker, bind via SQL). Plus
  `libself-audit.so`, an `LD_AUDIT` library that makes **stock glibc** load
  `.self` shared libraries resolved through SQL.
- `nix/` — packages, a `selfify` hook, a NixOS module (`programs.self`), and
  `self-vm`.
- `schema/self.sql` — the format DDL (generated from `selfconv/schema.py`).
- `examples/server/` — **self-httpd**: a webserver whose pages, program and
  visitor log are one file. It opens `argv[0]` as a database and serves out of
  its own `routes` table; editing the live site is an `UPDATE`. Live at
  <https://selfdb.exe.xyz>.
- `bench/`, `tests/` — the evaluation harness and the test suite.

Read [DESIGN.md](./DESIGN.md); §13 tracks implementation status.

## Try it

```console
$ nix develop                        # dev shell (converter + loader + tools)
$ nix develop -c bash tests/all.sh   # run M0..M3b end to end
$ nix develop -c bash tests/showcase.sh
$ nix run .#self-vm                  # a NixOS VM where ./hello.self just runs
                                     # (login: root, empty password)

$ bash examples/server/build.sh ./server   # a website, inserted into a program
$ loader/self-exec ./server 8080           # http://localhost:8080
```
