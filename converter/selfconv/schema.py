"""Authoritative DDL for the SELF format (v1).

`schema/self.sql` at the repo root is generated from this module:
    python -m selfconv.schema > schema/self.sql
"""

APPLICATION_ID = 0x53454C46  # 'SELF'
FORMAT_VERSION = 1

SCHEMA_SQL = f"""\
-- SELF: the Structured Executable & Linkable Format, v{FORMAT_VERSION}
-- A program is a SQLite database. This file is generated from
-- converter/selfconv/schema.py -- edit there.
--
-- PRAGMA application_id = 0x53454C46 ('SELF'), PRAGMA user_version = {FORMAT_VERSION}.
-- Identification: binfmt_misc magic 'SELF' at byte offset 68.

-- ── identity / ELF ehdr equivalent ──────────────────────────────
CREATE TABLE self_meta (
  key   TEXT PRIMARY KEY,
  value ANY
) WITHOUT ROWID;
-- rows: format_version, type ('EXEC'|'DYN') + et (int), machine ('x86_64')
--       + em (int), class (64), byte_order ('little'), osabi (int),
--       eflags (int), entry (int), phoff (int, image offset of the phdr
--       table), phnum, phentsize, interp (path or NULL), soname (or NULL),
--       build_id (hex or NULL), page_size, source (provenance path),
--       created_by

-- ── the program image ───────────────────────────────────────────
-- One row per program header, in original order. 'load' rows carry the
-- segment bytes; other rows describe ranges that live inside some load
-- segment (their content is NULL). `offset` is the file offset of the
-- segment in the *reconstructed image* (elf2self preserves the original
-- layout so that offset<->vaddr congruence, and the phdr table embedded in
-- the first load segment, survive round-trips).
CREATE TABLE segments (
  id      INTEGER PRIMARY KEY,   -- original phdr index
  type    TEXT NOT NULL,         -- 'load'|'dynamic'|'interp'|'tls'|'stack'
                                 --  |'relro'|'note'|'phdr'|'eh_frame'
                                 --  |'property'|'other'
  ptype   INTEGER NOT NULL,      -- raw ELF p_type
  offset  INTEGER NOT NULL,      -- image file offset (p_offset)
  vaddr   INTEGER NOT NULL,
  filesz  INTEGER NOT NULL,
  memsz   INTEGER NOT NULL,
  r       INTEGER NOT NULL DEFAULT 1,
  w       INTEGER NOT NULL DEFAULT 0,
  x       INTEGER NOT NULL DEFAULT 0,
  align   INTEGER NOT NULL DEFAULT 4096,
  content BLOB                   -- filesz bytes; NULL for non-load rows
);

-- ── dynamic linking ─────────────────────────────────────────────
CREATE TABLE needed (            -- DT_NEEDED, ordered
  ord    INTEGER PRIMARY KEY,
  soname TEXT NOT NULL
);

CREATE TABLE dynamic_entries (   -- the rest of PT_DYNAMIC, k/v
  tag   TEXT NOT NULL,           -- 'INIT_ARRAY', 'FLAGS_1', 'RUNPATH', ...
  value ANY
);

CREATE TABLE symbols (
  id      INTEGER PRIMARY KEY,
  name    TEXT NOT NULL,
  version TEXT,                  -- 'GLIBC_2.2.5' -- versioning as a COLUMN
  value   INTEGER,
  size    INTEGER,
  type    TEXT,                  -- 'func' | 'object' | 'tls' | ...
  bind    TEXT,                  -- 'global' | 'weak' | 'local'
  visibility TEXT,
  shndx   INTEGER,
  defined INTEGER NOT NULL,      -- 1 = defined here, 0 = import
  exported INTEGER NOT NULL,
  source  TEXT NOT NULL DEFAULT 'dynsym'  -- 'dynsym' | 'symtab'
);
CREATE INDEX idx_symbols_name ON symbols(name, version);
-- ^ this index IS .gnu.hash.

CREATE TABLE relocations (
  id     INTEGER PRIMARY KEY,
  offset INTEGER NOT NULL,       -- where to patch (vaddr)
  type   TEXT NOT NULL,          -- 'JUMP_SLOT', 'GLOB_DAT', 'RELATIVE', ...
  rtype  INTEGER NOT NULL,       -- raw relocation type number
  symbol INTEGER REFERENCES symbols(id),
  addend INTEGER NOT NULL DEFAULT 0
);

-- ── optional layers (strip(1) = DELETE + VACUUM) ────────────────
CREATE TABLE sections (          -- tooling-level; NOT used by any loader
  id INTEGER PRIMARY KEY,
  name TEXT, type TEXT, stype INTEGER, flags INTEGER,
  vaddr INTEGER, offset INTEGER, size INTEGER, entsize INTEGER,
  link INTEGER, info INTEGER, align INTEGER,
  content BLOB                   -- NULL for SHT_NOBITS
);
CREATE TABLE notes (kind TEXT, name TEXT, content BLOB);
CREATE TABLE docs  (topic TEXT PRIMARY KEY, body TEXT);

-- ── the self-describing part: views shipped in every binary ────
CREATE VIEW exports AS
  SELECT name, version, type, size, value FROM symbols
  WHERE exported = 1 AND source = 'dynsym';
CREATE VIEW imports AS
  SELECT name, version FROM symbols
  WHERE defined = 0 AND source = 'dynsym';
CREATE VIEW ldd AS SELECT ord, soname FROM needed ORDER BY ord;
"""

if __name__ == "__main__":
    print(SCHEMA_SQL, end="")
