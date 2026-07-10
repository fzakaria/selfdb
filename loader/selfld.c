/*
 * selfld.c: the M3b eager SQL binder.
 *
 * The full inversion: instead of handing off to glibc's ld.so (M2), we ARE
 * the dynamic linker. We map the main program and every NEEDED library --
 * each resolved through the resolver DB (SQL, not RUNPATH) and read straight
 * from its `segments` rows -- then bind eagerly by walking each object's
 * `relocations` JOIN `symbols` and resolving names against the indexed union
 * of every object's exports. `ldd` is a query; symbol resolution is a JOIN.
 *
 * Scope (per DESIGN.md §5): a curated, freestanding (no-libc) closure with
 * eager binding and no dlopen. Running glibc itself without its own rtld
 * (IFUNC/TLS/__libc_early_init) is deliberately out of scope; M3a already
 * carries the system-database demo on real glibc programs.
 *
 * x86-64 only.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sqlite3.h>

#include "image.h"
#include "self.h"

#define PAGE 4096UL
#define ALIGN_DOWN(x) ((x) & ~(PAGE - 1))
#define ALIGN_UP(x) ALIGN_DOWN((x) + PAGE - 1)

/* R_X86_64 relocation type numbers (stored raw in relocations.rtype). */
#define R_64 1
#define R_GLOB_DAT 6
#define R_JUMP_SLOT 7
#define R_RELATIVE 8

struct sym {
	char *name;
	uintptr_t addr;
	struct sym *next;
};
static struct sym *g_syms; /* global export table: name -> runtime addr */

static void export_add(const char *name, uintptr_t addr) {
	struct sym *s = malloc(sizeof *s);
	s->name = strdup(name);
	s->addr = addr;
	s->next = g_syms;
	g_syms = s;
}
static uintptr_t export_find(const char *name) {
	for (struct sym *s = g_syms; s; s = s->next)
		if (strcmp(s->name, name) == 0)
			return s->addr;
	return 0;
}

struct object {
	sqlite3 *db;
	uintptr_t load_bias;
	uintptr_t entry;
};

/* Map one .self object's PT_LOAD segments (reserving a hole for the PIE) and
 * record its exported dynamic symbols in the global table. */
static void map_object(const char *path, struct object *obj) {
	sqlite3 *db = self_db_open(path);
	if (!db)
		die("selfld: cannot open %s", path);
	obj->db = db;
	int f;
	int64_t entry = self_get_int(db, "entry", &f);

	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT vaddr, memsz FROM segments WHERE type='load'",
			   -1, &st, NULL);
	uint64_t min_v = UINT64_MAX, max_v = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		uint64_t v = sqlite3_column_int64(st, 0), m = sqlite3_column_int64(st, 1);
		if (v < min_v)
			min_v = v;
		if (v + m > max_v)
			max_v = v + m;
	}
	sqlite3_finalize(st);

	uintptr_t lo = ALIGN_DOWN(min_v), hi = ALIGN_UP(max_v);
	void *base = mmap(NULL, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		die("selfld: reserve span for %s: %m", path);
	uintptr_t bias = (uintptr_t)base - lo;

	sqlite3_prepare_v2(db,
	    "SELECT vaddr, filesz, memsz, r, w, x, content FROM segments"
	    " WHERE type='load' ORDER BY id", -1, &st, NULL);
	while (sqlite3_step(st) == SQLITE_ROW) {
		uint64_t vaddr = sqlite3_column_int64(st, 0);
		uint64_t filesz = sqlite3_column_int64(st, 1);
		uint64_t memsz = sqlite3_column_int64(st, 2);
		int r = sqlite3_column_int(st, 3), w = sqlite3_column_int(st, 4),
		    x = sqlite3_column_int(st, 5);
		const void *blob = sqlite3_column_blob(st, 6);
		int blen = sqlite3_column_bytes(st, 6);
		void *buf = NULL;
		if (blob && blen) {
			buf = malloc(blen);
			memcpy(buf, blob, blen);
		}
		uintptr_t start = ALIGN_DOWN(bias + vaddr), end = ALIGN_UP(bias + vaddr + memsz);
		if (mmap((void *)start, end - start, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
			die("selfld: map segment @%#lx: %m", start);
		if (buf)
			memcpy((void *)(bias + vaddr), buf, filesz);
		free(buf);
		/* leave writable until after relocation; hardened later */
		int prot = (r ? PROT_READ : 0) | PROT_WRITE | (x ? PROT_EXEC : 0);
		mprotect((void *)start, end - start, prot);
	}
	sqlite3_finalize(st);

	obj->load_bias = bias;
	obj->entry = f ? bias + entry : 0;

	/* publish this object's exported dynamic symbols */
	sqlite3_prepare_v2(db,
	    "SELECT name, value FROM symbols WHERE exported=1 AND source='dynsym'"
	    " AND value IS NOT NULL", -1, &st, NULL);
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(st, 0);
		uint64_t value = sqlite3_column_int64(st, 1);
		if (name && *name)
			export_add(name, bias + value);
	}
	sqlite3_finalize(st);
}

/* Apply an object's relocations, resolving symbols against the global table
 * (which is exactly `relocations` JOIN `symbols` over the loaded closure). */
static void relocate(struct object *obj, const char *path) {
	sqlite3_stmt *st;
	sqlite3_prepare_v2(obj->db,
	    "SELECT r.offset, r.rtype, r.addend, s.name"
	    " FROM relocations r LEFT JOIN symbols s ON r.symbol = s.id",
	    -1, &st, NULL);
	while (sqlite3_step(st) == SQLITE_ROW) {
		uint64_t off = sqlite3_column_int64(st, 0);
		int rtype = sqlite3_column_int(st, 1);
		int64_t addend = sqlite3_column_int64(st, 2);
		const char *name = (const char *)sqlite3_column_text(st, 3);
		uint64_t *slot = (uint64_t *)(obj->load_bias + off);
		switch (rtype) {
		case R_RELATIVE:
			*slot = obj->load_bias + addend;
			break;
		case R_GLOB_DAT:
		case R_JUMP_SLOT:
		case R_64: {
			uintptr_t s = name ? export_find(name) : 0;
			if (!s)
				die("selfld: %s: unresolved symbol '%s'", path,
				    name ? name : "(null)");
			*slot = s + (rtype == R_JUMP_SLOT ? 0 : addend);
			break;
		}
		default:
			die("selfld: %s: unhandled reloc type %d", path, rtype);
		}
	}
	sqlite3_finalize(st);
}

/* Minimal initial stack for a freestanding _start: argc/argv/envp/auxv. */
static uintptr_t build_stack(int argc, char **argv, char **envp,
			     uintptr_t entry, uintptr_t phdr, uint64_t phnum) {
	size_t sz = 1 << 20;
	uint8_t *stk = mmap(NULL, sz, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stk == MAP_FAILED)
		die("selfld: mmap stack: %m");
	uint8_t *p = stk + sz;

	int envc = 0;
	while (envp[envc])
		envc++;
	uintptr_t *ap = calloc(argc, sizeof *ap), *ep = calloc(envc ? envc : 1, sizeof *ep);
	for (int i = 0; i < argc; i++) {
		size_t n = strlen(argv[i]) + 1;
		p -= n;
		memcpy(p, argv[i], n);
		ap[i] = (uintptr_t)p;
	}
	for (int i = 0; i < envc; i++) {
		size_t n = strlen(envp[i]) + 1;
		p -= n;
		memcpy(p, envp[i], n);
		ep[i] = (uintptr_t)p;
	}
	uint8_t rnd[16];
	void *kr = (void *)getauxval(AT_RANDOM);
	memcpy(rnd, kr ? kr : (void *)"selfldselfld1234", 16);
	p -= 16;
	memcpy(p, rnd, 16);
	uintptr_t at_random = (uintptr_t)p;

	struct {
		uint64_t t, v;
	} aux[] = {
	    {AT_PHDR, phdr}, {AT_PHNUM, phnum}, {AT_PHENT, PHDR_SIZE},
	    {AT_PAGESZ, PAGE}, {AT_ENTRY, entry}, {AT_BASE, 0},
	    {AT_RANDOM, at_random}, {AT_SYSINFO_EHDR, getauxval(AT_SYSINFO_EHDR)},
	    {AT_NULL, 0},
	};
	int naux = sizeof(aux) / sizeof(aux[0]);
	size_t words = 1 + argc + 1 + envc + 1 + (size_t)2 * naux;
	uintptr_t sp = ((uintptr_t)p - words * 8) & ~(uintptr_t)15;
	uint64_t *v = (uint64_t *)sp;
	size_t k = 0;
	v[k++] = argc;
	for (int i = 0; i < argc; i++)
		v[k++] = ap[i];
	v[k++] = 0;
	for (int i = 0; i < envc; i++)
		v[k++] = ep[i];
	v[k++] = 0;
	for (int i = 0; i < naux; i++) {
		v[k++] = aux[i].t;
		v[k++] = aux[i].v;
	}
	free(ap);
	free(ep);
	return sp;
}

static __attribute__((noreturn)) void enter(uintptr_t sp, uintptr_t entry) {
	__asm__ volatile("mov %0, %%rsp\n\t"
			 "xor %%rdx, %%rdx\n\t"
			 "xor %%rbp, %%rbp\n\t"
			 "jmp *%1\n\t"
			 : : "r"(sp), "r"(entry) : "memory");
	__builtin_unreachable();
}

int run_selfld(sqlite3 *maindb, const char *path, char **argv, char **envp) {
	int64_t em = self_meta_int(maindb, "em");
	if (em != EM_X86_64)
		die("selfld supports x86-64 only (em=%ld)", em);
	sqlite3_close(maindb); /* map_object reopens by path uniformly */

	const char *dbpath = getenv("SELF_SYSTEM_DB");
	sqlite3 *rdb = NULL;
	if (dbpath)
		sqlite3_open_v2(dbpath, &rdb, SQLITE_OPEN_READONLY, NULL);

	/* map the main program */
	struct object main_obj;
	map_object(path, &main_obj);

	/* map its NEEDED closure (one level; the demo closure is flat) */
	struct object libs[32];
	int nlibs = 0;
	sqlite3_stmt *st;
	sqlite3_prepare_v2(main_obj.db, "SELECT soname FROM needed ORDER BY ord",
			   -1, &st, NULL);
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *soname = (const char *)sqlite3_column_text(st, 0);
		char *libpath = NULL;
		if (rdb) {
			sqlite3_stmt *q;
			sqlite3_prepare_v2(rdb,
			    "SELECT path FROM objects WHERE soname=? LIMIT 1", -1, &q, NULL);
			sqlite3_bind_text(q, 1, soname, -1, SQLITE_STATIC);
			if (sqlite3_step(q) == SQLITE_ROW)
				libpath = strdup((const char *)sqlite3_column_text(q, 0));
			sqlite3_finalize(q);
		}
		if (!libpath)
			die("selfld: cannot resolve NEEDED '%s' (set SELF_SYSTEM_DB)", soname);
		fprintf(stderr, "[selfld] %s -> %s\n", soname, libpath);
		map_object(libpath, &libs[nlibs++]);
		free(libpath);
	}
	sqlite3_finalize(st);

	/* eager binding: relocate every object now that all exports are known */
	for (int i = 0; i < nlibs; i++)
		relocate(&libs[i], "lib");
	relocate(&main_obj, path);

	uintptr_t sp = build_stack(1, argv, envp, main_obj.entry, 0, 0);
	enter(sp, main_obj.entry);
}
