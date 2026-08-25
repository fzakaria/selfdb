# SELF: the Structured Executable & Linkable Format

*An executable format that is a SQLite database — and a plan to boot real
programs with it on NixOS.*

> **Status**: M0–M3b implemented and tested (`nix develop -c bash tests/all.sh`);
> see [§13 Implementation status](#13-implementation-status) for what's real,
> what deviated from this plan, and what's still a stretch. The prose below is
> the original design; §13 reconciles it with the code.
> **Prior art by us**: [sqlelf](https://github.com/fzakaria/sqlelf) /
> [arXiv:2405.03883](https://arxiv.org/abs/2405.03883), which put a SQL *view*
> over ELF. SELF inverts that: the database *is* the format, and ELF becomes
> the derived artifact (or disappears entirely).

---

## 1. Thesis

ELF is a hand-rolled, offset-addressed database from 1989:

| ELF mechanism | What it actually is | The database primitive it reinvents |
|---|---|---|
| `.strtab` / `.dynstr` | string interning | `TEXT` columns + interning SQLite already does |
| `.hash` / `.gnu.hash` | symbol lookup acceleration | `CREATE INDEX` (b-tree) |
| section header table | table-of-tables | `sqlite_schema` |
| `st_name → strtab`, `r_sym → symtab` | foreign keys by array index | `REFERENCES` |
| `sh_offset`/`sh_size`/`sh_entsize` | fixed-width record packing | the record format of a b-tree page |
| `objcopy --strip-debug` | fragile offset-rewriting surgery | `DELETE FROM dwarf; VACUUM;` |
| `ldconfig` cache, `debuginfod`, build-id symlink farms | out-of-band system indexes | one more table, or `ATTACH` |
| "we can't extend ELF, every consumer hardcodes offsets" | no schema evolution | `ALTER TABLE ADD COLUMN`, views, app-defined tables |

Every ELF consumer (kernel, ld.so, binutils, LIEF, goblin, ...) re-implements
the same parser, and every producer re-implements the same serializer, because
the format's *data structures* are welded to its *encoding*. SQLite is the
counter-example the industry already trusts: a stable, documented,
single-file, mmap-able container with a query engine, used as an
[application file format](https://sqlite.org/appfileformat.html) precisely to
escape this trap. The demo goal: **show that a linked program is just rows,
and that once it's rows, an entire class of tooling (readelf, nm, ldd,
strings, ldconfig, strip, debuginfod) collapses into SQL one-liners — while
the programs still actually run.**

This is the [nushell](https://www.nushell.sh/) argument applied to binaries:
structured data at the boundary beats bytes + a bespoke parser at every
consumer.

### Non-goals (v0)

- Performance parity with ELF (we measure the gap honestly instead; §9).
- Kernel upstreaming. The kernel work is a stretch goal / conversation piece.
- Replacing kernel modules, vDSO, or firmware. Userspace programs and
  shared libraries only.
- A new object file format for compilers/`.o` files. We convert *post-link*
  output; the toolchain is untouched (a key demo constraint — see §6).

---

## 2. Bird's-eye view

```
                 nixpkgs (unmodified gcc/ld)
                          │ normal ELF link
                          ▼
   ┌──────────┐    ┌─────────────┐     ┌───────────────────────────┐
   │  hello   │───▶│  elf2self   │────▶│ hello.self  (SQLite DB,   │
   │  (ELF)   │    │ (converter) │     │  application_id = 'SELF') │
   └──────────┘    └─────────────┘     └────────────┬──────────────┘
                                                    │ execve("hello.self")
                                                    ▼
                                      kernel binfmt_misc: magic 'SELF' @68
                                                    │
                                                    ▼
                                       ┌────────────────────────┐
                                       │ self-exec (interpreter)│
                                       │  M1: rebuild ELF into  │
                                       │      memfd, fexecve    │
                                       │  M2: map segments, set │
                                       │      up stack, jump    │
                                       │  M3: SQL-native dynamic│
                                       │      linking via       │
                                       │      system.db / ATTACH│
                                       └────────────────────────┘
```

Three deliverable layers, each independently demo-able:

1. **Format + converter** (`elf2self`, `self2elf`, `self` CLI) — the schema,
   round-trip fidelity, and the "SQL beats readelf" showcase.
2. **Execution** (`self-exec` registered via binfmt_misc) — `./hello.self`
   just works; milestones M1→M3 make the execution progressively more
   "native" (§5).
3. **System integration** (nix flake + NixOS module + micro-VM) — a NixOS VM
   where selected packages ship as `.self`, plus the system-wide database
   (§7).

---

## 3. The container: why SQLite, concretely

- **Stable, documented on-disk format** ([fileformat.html](https://sqlite.org/fileformat2.html)),
  backwards compatible since 2004 — a *better* longevity story than most
  bespoke formats, and the reason an eventual in-kernel reader (§8) is even
  thinkable.
- **Single file, zero-copy-ish access**: b-tree pages are read on demand;
  with `PRAGMA mmap_size` the pager reads through a shared file mapping.
- **The header is binfmt-friendly**. SQLite reserves a 4-byte big-endian
  **`application_id` at byte offset 68** (`PRAGMA application_id`). We claim:

  ```
  application_id = 0x53454C46   -- 'SELF'
  user_version   = <format version, integer>
  ```

  `binfmt_misc` matches magic at a fixed offset within the first
  `BINPRM_BUF_SIZE = 256` bytes — offset 68 is comfortably inside. For extra
  robustness the registration can use one 72-byte magic starting at offset 0
  ("`SQLite format 3\0`" … `SELF`) with a mask zeroing bytes 16–67.
  Ordinary SQLite databases (application_id 0) are never picked up, and the
  execute permission bit still gates everything, as with any binfmt.
- **Tooling for free**: `sqlite3`, `.schema`, datasette, every language
  binding on earth, `sqldiff` (semantic binary diffing!), `.recover`,
  `sqlite3_analyzer` (what's taking up space — a better `bloaty`).
- **Transactions**: `strip`, `patchelf --set-rpath`, adding a signature —
  all become *atomic, crash-safe, in-place* operations. patchelf's entire
  reason to exist (growing a string table requires rewriting the file) is a
  non-problem.

### File identity

- Extension `.self` by convention; the kernel doesn't care (magic match).
- `PRAGMA page_size = 4096` at creation — aligns pager pages with the CPU
  page size, and matters for the mmap story in M2 (§5).

---

## 4. The schema (format v0)

Design rules:

1. **`segments` is the load-bearing table.** Execution must need *only*
   `self_meta` + `segments` (+ `needed`/`relocations`/`symbols` for dynamic
   linking in M3). Everything else is optional and strippable.
2. **No string tables, no hand-rolled hashes** — names are `TEXT`, lookup
   acceleration is `CREATE INDEX`.
3. **Stay recognizable to sqlelf.** Column names follow sqlelf's virtual
   tables (`symbols`, `sections`, `dynamic_entries`, ...) so existing queries
   port with minimal edits; where sqlelf keyed rows by `path`, the
   single-object file drops it and the *system* database (§7) reintroduces it.
4. **The file describes itself.** Views, `CHECK` constraints, and a `docs`
   table ship *inside* every binary. `.schema` is the spec.

```sql
PRAGMA application_id = 0x53454C46;  -- 'SELF'
PRAGMA user_version   = 1;           -- format version
PRAGMA page_size      = 4096;

-- ── identity / ELF ehdr equivalent ──────────────────────────────
CREATE TABLE self_meta (
  key   TEXT PRIMARY KEY,
  value ANY
) WITHOUT ROWID;
-- rows: format_version, type ('EXEC'|'DYN'), machine ('x86_64', ...),
--       class (64), byte_order ('little'), os_abi ('sysv'),
--       entry (int), page_size, soname (nullable), build_id (blob),
--       interp_semantics ('self-exec/v1'), created_by ('elf2self 0.1'),
--       source ('/nix/store/...-hello-2.12.1/bin/hello')   -- provenance!

-- ── the program image: PT_LOAD/PT_TLS/PT_GNU_STACK equivalents ──
CREATE TABLE segments (
  id      INTEGER PRIMARY KEY,
  type    TEXT NOT NULL,          -- 'load' | 'tls' | 'stack' | 'relro'
  vaddr   INTEGER NOT NULL,
  memsz   INTEGER NOT NULL,
  filesz  INTEGER NOT NULL,
  r       INTEGER NOT NULL DEFAULT 1,
  w       INTEGER NOT NULL DEFAULT 0,
  x       INTEGER NOT NULL DEFAULT 0,
  align   INTEGER NOT NULL DEFAULT 4096,
  content BLOB                    -- filesz bytes; NULL for pure-BSS
);

-- ── dynamic linking ─────────────────────────────────────────────
CREATE TABLE needed (             -- DT_NEEDED, ordered
  ord    INTEGER PRIMARY KEY,
  soname TEXT NOT NULL
);

CREATE TABLE dynamic_entries (    -- the rest of PT_DYNAMIC, k/v
  tag   TEXT NOT NULL,            -- 'INIT_ARRAY', 'FLAGS_1', 'RUNPATH', ...
  value ANY
);

CREATE TABLE symbols (
  id      INTEGER PRIMARY KEY,
  name    TEXT NOT NULL,
  version TEXT,                   -- 'GLIBC_2.2.5' — versioning as a COLUMN,
                                  -- not the .gnu.version_r contraption
  value   INTEGER,
  size    INTEGER,
  type    TEXT,                   -- 'func' | 'object' | 'tls' | ...
  bind    TEXT,                   -- 'global' | 'weak' | 'local'
  visibility TEXT,
  defined INTEGER NOT NULL,       -- 1 = exported/defined, 0 = import
  exported   INTEGER NOT NULL
);
CREATE INDEX idx_symbols_name ON symbols(name, version);
-- ^ this index IS .gnu.hash. That's the whole slide.

CREATE TABLE relocations (
  id     INTEGER PRIMARY KEY,
  offset INTEGER NOT NULL,        -- where to patch (vaddr)
  type   TEXT NOT NULL,           -- 'R_X86_64_GLOB_DAT', 'JUMP_SLOT', ...
  symbol INTEGER REFERENCES symbols(id),
  addend INTEGER NOT NULL DEFAULT 0
);

-- ── optional layers (strippable = DELETE + VACUUM) ──────────────
CREATE TABLE sections (           -- tooling-level; NOT used by the loader
  name TEXT, type TEXT, flags TEXT, vaddr INTEGER, size INTEGER,
  segment INTEGER REFERENCES segments(id), content BLOB
);
CREATE TABLE notes (kind TEXT, name TEXT, content BLOB);
CREATE TABLE docs  (topic TEXT PRIMARY KEY, body TEXT);  -- format docs, man page?
-- future: dwarf_dies / dwarf_lines (sqlelf already generates these),
--         signatures(scope, algo, signature BLOB), sbom(...)

-- ── the self-describing part: views shipped in every binary ────
CREATE VIEW exports AS
  SELECT name, version, type, size FROM symbols WHERE exported = 1;
CREATE VIEW imports AS
  SELECT name, version FROM symbols WHERE defined = 0;
CREATE VIEW ldd AS SELECT ord, soname FROM needed ORDER BY ord;
```

Deliberate omissions from ELF, with rationale:

- **No section↔segment address arithmetic** — `sections.segment` is an
  explicit FK, not an overlap computation every tool re-derives.
- **No PLT/GOT description beyond relocations** — M1/M2 keep glibc's ld.so in
  charge so the PLT machinery lives in `segments` content untouched; M3
  binds eagerly (`BIND_NOW` semantics), which modern hardened distros default
  to anyway.
- **No `strings` table** — sqlelf's `strings` was a *view over* `.dynstr`;
  here strings are just... text in columns. `SELECT name FROM symbols` is
  `nm`; there is nothing left for `strings(1)` to grovel.

### Round-trip requirement

`self2elf(elf2self(x))` must produce a *functionally equivalent* ELF (same
segments, dynamic info, symbols — not byte-identical). This is the
correctness anchor for the converter and CI's main check, and it proves the
schema captures everything execution needs.

---

## 5. Execution: three milestones

The binfmt registration is identical throughout; only `self-exec` grows.
Each milestone is a complete, honest demo.

### M1 — "it runs": rebuild ELF in a memfd (~a day of work)

`self-exec hello.self`:

1. `sqlite3_open_v2(READONLY)`, sanity-check `application_id`/`user_version`.
2. Reconstruct a minimal ELF **from the tables** (ehdr from `self_meta`,
   phdrs + contents from `segments`, PT_INTERP → the real ld.so from the
   converted binary's original interp, dynamic section from
   `needed`/`dynamic_entries`): write into `memfd_create()`.
3. `fexecve(memfd, argv, envp)`.

- Shared libraries stay ELF; only executables are converted. Everything
  dynamic "just works" because glibc's ld.so takes over.
- **Crucially, this is not "ELF blob smuggled in a database"** — the ELF is
  *re-serialized from rows* at exec time. The tables are authoritative;
  `self2elf` and M1's step 2 are the same code.
- Known costs: no page-cache sharing of text between processes; +1 exec.
  Fine for a demo; measured in §9.

### M2 — "it loads": be the loader (the meaty part)

`self-exec` maps the image itself, the way `fs/binfmt_elf.c` +
`ld.so` would:

1. Pick a base (PIE: mmap-reserve `max(vaddr+memsz)`), then per `segments`
   row: `mmap(PROT_WRITE)` anonymous, `sqlite3_blob_read()` the content in
   (the incremental-blob API — no full-row allocation), `mprotect` to r/w/x.
2. Static-PIE binaries first: set up the initial stack (argv/envp/auxv with
   `AT_ENTRY`, `AT_PHDR`, `AT_RANDOM`, ...), jump to `entry`. This is
   ~the classic "userspace exec" exercise; prior art abounds
   (`libreflect`/grugq's ul_exec).
3. Dynamic binaries: map glibc's ld.so (an ELF, read via plain mmap)
   ourselves, point `AT_BASE`/`AT_PHDR` at a synthesized phdr table for the
   main image, jump to ld.so's entry. ld.so then handles NEEDED/relocs as
   usual — it never knows the main program came from a database.

Notes:

- The synthesized phdrs must live in memory (glibc reads them via
  `AT_PHDR`) — small anonymous mapping, no file needed.
- vDSO/`AT_SYSINFO_EHDR` comes free (it's our own auxv, inherited).
- TLS segment (`type='tls'`) must be forwarded into the phdr table.
- W^X: content is written before `mprotect(PROT_EXEC)`; no WX window.

### M3 — "the system is a database": SQL-native dynamic linking

Convert *libraries* too. **glibc is the target** — it's what essentially all
of nixpkgs links against, so a demo that only works on musl doesn't prove
the thesis. That constraint splits M3 into a compatible half and an
ambitious half:

**M3a — SQL resolution, glibc relocation (works for arbitrary packages).**
glibc's rtld-audit interface (`LD_AUDIT`, `la_objsearch`) lets an audit
library intercept every soname→path resolution — including `dlopen` — before
any filesystem search happens. So:

- `self-exec` injects `libself-audit.so`; its `la_objsearch` resolves
  `needed.soname` by querying the **resolver database** (§7) instead of
  walking RUNPATH directories:
  `SELECT path FROM objects WHERE soname = ? AND machine = ?`.
- If the resolved object is a `.self` library, the audit lib materializes it
  as an ELF in a `memfd` (the *same* row→ELF serializer M1 uses for the main
  program) and returns `/proc/self/fd/N`. Stock ld.so maps and relocates it,
  none the wiser.
- Result: unmodified glibc, unmodified packages, lazy PLT, IFUNCs, TLS,
  symbol versioning all keep working — while library *storage* is rows and
  library *lookup* is SQL. `ldconfig`'s cache is now an indexed table, and
  the memfd cache can be keyed by `build_id` (itself a query).

**M3b — SQL binding: `self-ld` replaces ld.so for a curated closure.**
The full inversion — map each library's `segments`, bind eagerly by
iterating `relocations` JOIN `symbols`, resolving through the indexed union
of loaded objects' `exports` in breadth-first scope order, mimicking ld.so:

  ```sql
  SELECT s.value, o.base FROM scope o
  JOIN symbols s USING (object)
  WHERE s.name = ? AND s.exported = 1
  ORDER BY o.load_order LIMIT 1;
  ```

Scope: eager binding only (`BIND_NOW` semantics), no dlopen, a curated
closure (`hello` + converted glibc). Running glibc code without its own rtld
is the known-hard part; the specific dragons, so we plan rather than
discover them: IFUNC relocations (`R_X86_64_IRELATIVE` — must run resolver
functions at bind time), static TLS layout and TCB setup, symbol versioning
in lookup order, and glibc's libc↔rtld handshake (`__libc_early_init`,
`_rtld_global` — self-ld must export or stub the `ld.so` ABI libc expects).
If a dragon proves lethal, M3b degrades gracefully: M3a already delivers the
system-database demo on real packages, and M3b can fall back to
demonstrating on a static-PIE + one converted leaf library rather than
switching libcs.

M3 is where the pitch lands: **`ldd` is a recursive CTE, `ldconfig` is an
index, and "which library will actually satisfy this symbol?" — today a
gdb-or-suffering question — is a JOIN.**

### Registration (all milestones)

```nix
boot.binfmt.registrations.self = {
  recognitionType = "magic";
  offset = 68;
  magicOrExtension = "SELF";
  interpreter = "${self-exec}/bin/self-exec";
  openBinary = true;        # 'O': pass an fd — works in containers,
  fixBinary  = true;        # 'F': resolve interp at register time (no TOCTOU)
  preserveArgvZero = true;  # 'P': keep argv[0] semantics
};
```

`self-exec` itself must remain ELF (a matching interpreter recurses to
`-ELOOP` — we learned this on the binfmt-BPF work).

---

## 6. Producing SELF binaries in nixpkgs (no toolchain changes)

The whole point of using nixpkgs: we can rebuild the world — and therefore we
should barely have to.

- **`elf2self`**: Python + LIEF, lifted from sqlelf's extractors (they
  already enumerate headers/segments/symbols/relocs/versions into rows;
  the delta is writing rows to a real DB instead of virtual tables, plus the
  segment `content` blobs). A later C rewrite (libelf + libsqlite3) only if
  closure size for the hook demands it.
- **`selfifyHook`**: a `postFixup` hook — for each ELF executable in
  `$out/bin`, run `elf2self`, replace the file, keep the x-bit. Opt-in per
  package via an overlay:

  ```nix
  self: super: {
    hello-self = selfify super.hello;         # selfify = drv: addHook drv
    coreutils-self = selfify super.coreutils; # stretch: a full userland
  }
  ```

- **Untouched**: gcc, binutils, glibc, the nix daemon. Stretch-only ideas if
  we ever want "born as SELF" rather than converted: an `ld` wrapper that
  runs `elf2self` inline (trivial, still no toolchain rebuild), or a mold
  output plugin (big; not worth it for the thesis).
- Big builds (if we do end up rebuilding glibc/musl variants or a kernel):
  `--builders 'ssh-ng://leviathan.cymric-daggertooth.ts.net'`.

### NixOS demo VM

Reuse the micro-VM pattern from the binfmt-BPF flake: a
`nixosConfigurations.self-vm` with the binfmt module above, `hello-self` et
al. in `environment.systemPackages`, and a oneshot demo service that runs the
showcase script (§10). `nix run .#self-vm` → a shell where `hello` is a
database and doesn't know it.

---

## 7. The system-wide database

Two composable mechanisms — we implement (a) and demo (b) on top of it:

**(a) Resolver DB** `/var/lib/self/system.db` — the ldconfig replacement.
A NixOS activation script (or nix post-build hook) upserts:

```sql
CREATE TABLE objects (
  id INTEGER PRIMARY KEY,
  path TEXT UNIQUE,            -- /nix/store/...-glibc/lib/libc.so.6.self
  build_id BLOB, soname TEXT, machine TEXT
);
CREATE INDEX idx_soname ON objects(soname, machine);
-- plus mirrored (object_id, name, version) exports for cross-system queries
```

**(b) One logical database via ATTACH.** Because every `.self` file is a
SQLite DB with the *same schema*, system-wide queries need no new format:

```sql
ATTACH '/nix/store/...-hello/bin/hello' AS hello;
ATTACH '/nix/store/...-glibc/lib/libc.so.6.self' AS libc;
SELECT h.name FROM hello.imports h
LEFT JOIN libc.exports l USING (name) WHERE l.name IS NULL;
-- "which of hello's imports does libc NOT satisfy?" — ldd -r, as a JOIN
```

Storing *everything* in literally one DB file (segments of all binaries as
rows keyed by store path) is possible and fun to mention — a whole userland
under `PRAGMA integrity_check`, binary dedup via content hashing, atomic
system updates as transactions — but it fights Nix's store model (per-path
immutability, hard links, signatures) for no demo value. The
resolver-DB + ATTACH combo delivers the same "the system is queryable"
punchline while each store path stays a self-contained file. Revisit if the
thesis evolves toward "the store is a database".

---

## 8. Kernel angle (stretch, and the tie-in to existing work)

Ordered by effort:

1. **None (default)**: binfmt_misc magic at offset 68 — works today,
   `CONFIG_BINFMT_MISC` only.
2. **binfmt_misc + BPF matcher**: the existing `sqlite3-binfmt`-adjacent
   branch (`binfmt-misc-bpf`, the `'B'` match type + `bpf_binprm_set_interp`)
   gets its killer example: a program that checks the SQLite magic *and*
   `application_id` *and* dispatches on `user_version` to versioned
   interpreters — exactly the "smarter-than-a-mask matching" the LKML
   series needs as motivation. Low effort, high narrative value.
3. **`fs/binfmt_self.c`**: an in-kernel loader that walks the SQLite b-tree
   read-only. Feasibility notes: the file format is stable/documented; a
   read-only cursor over table b-trees (no SQL, no VM — direct b-tree walk
   of `segments` by rowid, schema pinned by `user_version`) is realistically
   1–2k LOC of kernel C with no allocation weirdness. It would give real
   demand paging *of the DB file* via the page cache… but content blobs are
   not page-aligned inside pages, so text still can't be mapped
   shared-executable directly. Honest verdict: a fantastic talk slide and a
   terrible patch series; do it last, if ever, as `CONFIG_BINFMT_SELF`
   in-tree on the branch for the VM demo.

A middle path worth one experiment: **page-aligned blob layout**. SQLite
stores big blobs on overflow-page chains (4-byte next-pointer + 4092 bytes
payload per page) — so even with `page_size=4096`, content is contiguous
but *off by 4 per page*, killing direct mmap. Options: accept the copy
(M1/M2 do), or a `content_offset` column pointing into a
page-aligned reserved region appended past the last b-tree page (legal:
SQLite ignores nothing after the DB size in header… actually it doesn't —
this needs the `.self` file to be DB + trailing aligned payload, which
`sqlite3_open` tolerates only via careful header size bookkeeping). Keep as
an appendix experiment; if it works, M2 gets `MAP_SHARED` text and the
performance section gets much stronger.

---

## 9. Honest evaluation plan

Claims we make must survive measurement; the paper/talk needs both columns.

**Costs (measure, don't hide):**

| Metric | Method | Expectation |
|---|---|---|
| exec latency | `hyperfine` on fork/exec of hello (ELF vs M1 vs M2), cold & warm cache | M1 worst (+memfd +double exec); M2 within small multiple of ELF |
| file size | closure of hello/coreutils, ELF vs `.self`, ± `VACUUM`, ± optional tables | SQLite overhead amortizes; strippability may *win* on debug-included comparisons |
| memory sharing | `pss` across N concurrent instances | ELF wins (shared text) until the §8 aligned-blob trick |
| lookup perf | symbol resolution µbench: `.gnu.hash` vs SQLite index | same complexity class; constant factor measured |

**Wins (the demo script, §10, doubles as the ergonomics benchmark):** lines
of code / commands to answer real questions, sqlelf-paper style, e.g.
"find every binary on the system that imports `SSL_read` with version <
OPENSSL_3" — a shell-and-readelf odyssey vs one query.

**Correctness:** round-trip differential testing (run coreutils' test suite
against converted binaries under M1/M2); `sqldiff` between
`elf2self(self2elf(x))` and `x` must be empty.

---

## 10. The showcase (what we actually demo)

```console
$ file hello
hello: SQLite 3.x database, application id 0x53454C46, user version 1
$ ./hello                      # binfmt_misc → self-exec
Hello, world!
$ sqlite3 hello '.schema segments'
$ sqlite3 hello 'SELECT soname FROM ldd'
$ sqlite3 hello 'SELECT name, version FROM imports LIMIT 5'
$ sqlite3 hello 'DELETE FROM sections; DELETE FROM notes; VACUUM;'  # strip(1)
$ ./hello                      # still runs — optional tables were optional
Hello, world!
$ sqldiff hello hello-2.12.2   # semantic binary diff
$ sqlite3 /var/lib/self/system.db \
    'SELECT path FROM objects o JOIN exports e ON ... WHERE e.name="SSL_read"'
```

Each line is a slide. The strip-then-run one is the thesis in two commands.

---

## 11. Repository layout & roadmap

```
selfdb/
  DESIGN.md            # this file
  flake.nix            # devshell now; packages/nixosConfigurations as they land
  schema/self.sql      # authoritative DDL (v1)
  converter/           # elf2self / self2elf / self CLI  (python + LIEF)
  loader/              # self-exec (C, libsqlite3)
  nix/                 # selfifyHook, NixOS module, self-vm
  bench/               # hyperfine + size + pss harnesses
  examples/server/     # self-httpd: a webserver that is its own database
```

- **M0** (format): `schema/self.sql`, `elf2self`/`self2elf`, round-trip test
  on `hello`. Exit: showcase queries work.
- **M1** (runs): memfd `self-exec`, binfmt registration in a NixOS VM,
  converted `hello` + a few coreutils run. Exit: `./hello.self` from a shell.
- **M2** (loads): direct mapping, static-PIE then dynamic-via-mapped-ld.so.
  Exit: coreutils test suite passes; bench table filled.
- **M3** (links): M3a — `la_objsearch` audit resolver + `.self` libraries
  materialized via memfd, working on unmodified glibc packages. M3b —
  `self-ld` eager SQL binding for a curated glibc closure. Exit:
  `ldd`-as-CTE demo against converted libs, resolver DB live in the VM.
- **M4** (stretch): BPF-matcher tie-in; aligned-blob mmap experiment;
  `binfmt_self.c` on the kernel branch.

## 12. Open questions

- ~~**Name**~~ *resolved 2026-07-09*: `SELF` / `.self` it is (the Sony
  Signed-ELF collision is acceptable); `sqelf` is the approved fallback if
  grep-hostility ever grates.
- Should `segments.content` be zstd-compressed (a `compression` column)?
  Costs mmap dreams, wins size — maybe only for non-load tables.
- setuid `.self` binaries: binfmt_misc `C` flag semantics are subtle;
  probably declare out of scope explicitly.
- dlopen of `.self` libraries: covered for free in M3a (`la_objsearch`
  fires for dlopen too); explicitly out of scope for M3b's `self-ld`.
- Signatures table: minisign over a canonical serialization
  (`SELECT ... ORDER BY`) — worth designing early since "signing rows, not
  bytes" is a genuinely novel-feeling win.

---

## 13. Implementation status

What actually got built, and where it diverged from the plan above. Run it
all with `nix develop -c bash tests/all.sh`; boot the VM with
`nix run .#self-vm` (login root, empty password).

| Milestone | Status | Evidence |
|---|---|---|
| **M0** format + converter | ✅ done | `converter/selfconv/` (`elf2self`, `self2elf`, `self`), `schema/self.sql`; `tests/roundtrip.sh` round-trips `hello` + nixpkgs `ls`, strips via `DELETE`+`VACUUM`, still runs |
| **M1** memfd loader | ✅ done | `loader/self-exec.c` + `image.c`; `nix/module.nix` binfmt registration; VM boots and runs `./hello.self` → `SELF-DEMO-OK` |
| **M2** native loader | ✅ done | `loader/native.c` maps segments + ld.so, synth stack/auxv; `tests/loader.sh` runs `hello`/`ls`/`readlink` native; VM → `SELF-NATIVE-OK` |
| **M3a** LD_AUDIT resolver | ✅ done | `loader/audit.c` (`libself-audit.so`); `tests/audit.sh` deletes the ELF `libgreet`, runs it from SQLite via stock glibc |
| **M3b** self-ld binder | ✅ done (freestanding) | `loader/selfld.c`; `tests/selfld.sh` binds a no-libc app→lib closure via SQL, exit 42 |
| **M4** kernel / mmap | ⏸ not started | stretch; see §8 |
| **example** single-file webserver | ✅ done | `examples/server/`; the program opens `argv[0]` and serves out of `routes`, writing `visits`/`presses` back into itself. `tests/server.sh`; deployed at <https://selfdb.exe.xyz> |

### Deviations from the design

- **Schema.** `self_meta` is a real key/value table (as designed), but the
  load-bearing rows are richer than §4's sketch: `segments` keeps each
  program header's original **file `offset`** (not just vaddr), because the
  loaders reconstruct a byte-exact image so `AT_PHDR` and offset/vaddr
  congruence survive. `symbols` gained `shndx`/`source` (`dynsym` vs
  `symtab`) so `exports`/`imports` views can filter correctly. `relocations`
  stores both a readable `type` and the raw `rtype` number (self-ld needs the
  number; humans want the name). The authoritative DDL lives in
  `converter/selfconv/schema.py`, generated into `schema/self.sql`.
- **binfmt flags.** The design proposed `O`+`F`+`P`. We ship with **none of
  them**: `P` (preserve-argv0) makes the kernel inject the original `argv[0]`
  as an extra leading operand that strict programs (GNU hello) reject.
  Without it the kernel hands the interpreter `[self-exec, <path>, args…]`
  and `basename(<path>)` still satisfies multi-call binaries like coreutils.
- **M3b scope.** As anticipated in §5, `self-ld` targets a **freestanding
  (no-libc)** closure, not glibc-without-its-rtld. It handles
  `RELATIVE`/`GLOB_DAT`/`JUMP_SLOT`/`64` relocations and eager binding; TLS,
  IFUNC and the libc↔rtld handshake are out of scope. Real glibc programs
  get the "system is a database" treatment through **M3a** instead, which is
  the more useful half anyway.
- **Loader packaging.** The row→ELF serializer is factored into
  `loader/image.c` (exit-free) and shared by `self-exec` and the audit
  library; `self2elf` (Python) is its twin for round-trip testing.

### Benchmarks (this host)

See `bench/results.md`. Headline: memfd/native exec cost ~5× a bare exec
(0.42 ms → ~2.1 ms — the reconstruct + open-SQLite + interpreter constant),
and a **stripped** coreutils SELF (1.79 MB) lands within ~1% of the ELF
(1.77 MB). No shared text pages yet (§8), which is the real perf gap to close.
