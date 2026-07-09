# selfdb

**SELF** — the *Structured Executable & Linkable Format*: what if a program
were a SQLite database instead of an ELF file?

[sqlelf](https://github.com/fzakaria/sqlelf) ([arXiv:2405.03883](https://arxiv.org/abs/2405.03883))
put a SQL view over ELF. This project inverts it: the rows become the format,
a `binfmt_misc` interpreter executes them, and nixpkgs/NixOS is the vehicle
to run a real (slice of a) system on it.

Start with [DESIGN.md](./DESIGN.md).

```console
$ nix develop   # converter/loader dev shell
```
