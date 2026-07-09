/*
 * native.c: the M2 loader.
 *
 * Instead of reconstructing an ELF and letting the kernel's binfmt_elf map
 * it, we map the program's segments ourselves straight from the `segments`
 * rows, map the ELF interpreter (glibc ld.so) ourselves too, synthesize a
 * fresh initial stack (argc/argv/envp/auxv), and jump to ld.so's entry --
 * exactly the contract fs/binfmt_elf.c gives ld.so, so ld.so never knows
 * the program came out of a database. Static-PIE programs (no PT_INTERP)
 * are handled by jumping straight to their own entry.
 *
 * x86-64 only.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "self.h"

#define PAGE 4096UL
#define ALIGN_DOWN(x) ((x) & ~(PAGE - 1))
#define ALIGN_UP(x) ALIGN_DOWN((x) + PAGE - 1)

/* One mapped object: where it landed and the facts the auxv needs. */
struct object {
	uintptr_t load_bias; /* runtime_addr = p_vaddr + load_bias */
	uintptr_t entry;     /* runtime entry point */
	uintptr_t phdr;      /* runtime address of the program header table */
	uint64_t phnum, phent;
};

static int prot_of(int r, int w, int x) {
	return (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) | (x ? PROT_EXEC : 0);
}

/* Map one PT_LOAD-like span at load_bias+vaddr and copy `filesz` bytes of
 * `src` into it; the tail up to memsz is zero (fresh anonymous pages). */
static void map_segment(uintptr_t load_bias, uint64_t vaddr, uint64_t filesz,
			uint64_t memsz, const void *src, int prot) {
	uintptr_t start = ALIGN_DOWN(load_bias + vaddr);
	uintptr_t end = ALIGN_UP(load_bias + vaddr + memsz);
	void *p = mmap((void *)start, end - start, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED)
		die("mmap segment @%#lx (%#lx bytes): %m", start, end - start);
	if (filesz && src)
		memcpy((void *)(load_bias + vaddr), src, filesz);
	if (mprotect((void *)start, end - start, prot) != 0)
		die("mprotect segment @%#lx: %m", start);
}

/* Reserve a contiguous hole big enough for all of an object's PT_LOADs so
 * their relative offsets are preserved, and return the chosen load bias. */
static uintptr_t reserve_span(uint64_t min_vaddr, uint64_t max_vaddr, int is_pie) {
	if (!is_pie)
		return 0; /* ET_EXEC: fixed addresses, no bias */
	uintptr_t lo = ALIGN_DOWN(min_vaddr), hi = ALIGN_UP(max_vaddr);
	void *base = mmap(NULL, hi - lo, PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		die("reserve %#lx bytes: %m", hi - lo);
	return (uintptr_t)base - lo;
}

/* Map the ELF interpreter (ld.so) from a file on disk. */
static void map_interp(const char *path, struct object *out) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("open interpreter %s: %m", path);
	struct stat sb;
	if (fstat(fd, &sb) != 0)
		die("stat interpreter: %m");
	uint8_t *file = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (file == MAP_FAILED)
		die("mmap interpreter: %m");
	close(fd);

	Elf64_Ehdr *eh = (void *)file;
	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0)
		die("%s: not an ELF interpreter", path);
	Elf64_Phdr *ph = (void *)(file + eh->e_phoff);

	uint64_t min_v = UINT64_MAX, max_v = 0;
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD) {
			if (ph[i].p_vaddr < min_v)
				min_v = ph[i].p_vaddr;
			if (ph[i].p_vaddr + ph[i].p_memsz > max_v)
				max_v = ph[i].p_vaddr + ph[i].p_memsz;
		}
	/* ld.so is ET_DYN; give it its own hole. */
	uintptr_t bias = reserve_span(min_v, max_v, 1);
	for (int i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD)
			map_segment(bias, ph[i].p_vaddr, ph[i].p_filesz,
				    ph[i].p_memsz, file + ph[i].p_offset,
				    prot_of(ph[i].p_flags & PF_R,
					    ph[i].p_flags & PF_W,
					    ph[i].p_flags & PF_X));
	out->load_bias = bias;
	out->entry = bias + eh->e_entry; /* read before unmapping the file */
	munmap(file, sb.st_size);
}

/* Map the main program straight from the `segments` rows. */
static void map_program(sqlite3 *db, struct object *out) {
	int64_t et = self_meta_int(db, "et");
	int64_t entry = self_meta_int(db, "entry");
	int64_t phoff = self_meta_int(db, "phoff");
	int is_pie = (et == ET_DYN);

	/* pass 1: bounds over load segments */
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db,
	    "SELECT vaddr, memsz FROM segments WHERE type='load'", -1, &st, NULL);
	uint64_t min_v = UINT64_MAX, max_v = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		uint64_t v = sqlite3_column_int64(st, 0);
		uint64_t m = sqlite3_column_int64(st, 1);
		if (v < min_v)
			min_v = v;
		if (v + m > max_v)
			max_v = v + m;
	}
	sqlite3_finalize(st);
	uintptr_t bias = reserve_span(min_v, max_v, is_pie);

	/* pass 2: map each load segment and find where phoff lands */
	uintptr_t phdr_addr = 0;
	sqlite3_prepare_v2(db,
	    "SELECT offset, vaddr, filesz, memsz, r, w, x, content"
	    " FROM segments WHERE type='load' ORDER BY id", -1, &st, NULL);
	while (sqlite3_step(st) == SQLITE_ROW) {
		uint64_t off = sqlite3_column_int64(st, 0);
		uint64_t vaddr = sqlite3_column_int64(st, 1);
		uint64_t filesz = sqlite3_column_int64(st, 2);
		uint64_t memsz = sqlite3_column_int64(st, 3);
		int r = sqlite3_column_int(st, 4);
		int w = sqlite3_column_int(st, 5);
		int x = sqlite3_column_int(st, 6);
		const void *blob = sqlite3_column_blob(st, 7);
		int blen = sqlite3_column_bytes(st, 7);
		/* copy the blob out before it is invalidated by the next step */
		void *buf = NULL;
		if (blob && blen) {
			buf = malloc(blen);
			memcpy(buf, blob, blen);
		}
		map_segment(bias, vaddr, filesz, memsz, buf, prot_of(r, w, x));
		free(buf);
		if (off <= (uint64_t)phoff && (uint64_t)phoff < off + filesz)
			phdr_addr = bias + vaddr + (phoff - off);
	}
	sqlite3_finalize(st);

	out->load_bias = bias;
	out->entry = bias + entry;
	out->phdr = phdr_addr;
	out->phnum = self_meta_int(db, "phnum");
	out->phent = self_meta_int(db, "phentsize");
}

/* ---- initial stack construction ------------------------------------- */

struct stackbuf {
	uint8_t *base;
	size_t size;
	uint8_t *top; /* highest usable address */
	uint8_t *p;   /* string cursor, grows down from top */
};

static uintptr_t push_str(struct stackbuf *s, const char *str) {
	size_t n = strlen(str) + 1;
	s->p -= n;
	memcpy(s->p, str, n);
	return (uintptr_t)s->p;
}
static uintptr_t push_bytes(struct stackbuf *s, const void *b, size_t n) {
	s->p -= n;
	memcpy(s->p, b, n);
	return (uintptr_t)s->p;
}

/* Build [argc][argv..][NULL][envp..][NULL][auxv..][AT_NULL] and return the
 * pointer that must be in %rsp at entry (points at argc, 16-byte aligned). */
static uintptr_t build_stack(struct stackbuf *s, int argc, char **argv,
			     char **envp, const struct object *exe,
			     const struct object *interp, int have_interp,
			     const char *exec_path) {
	int envc = 0;
	while (envp[envc])
		envc++;

	/* place strings + aux data at the top */
	uintptr_t *argp = calloc(argc, sizeof(uintptr_t));
	for (int i = 0; i < argc; i++)
		argp[i] = push_str(s, argv[i]);
	uintptr_t *envpp = calloc(envc, sizeof(uintptr_t));
	for (int i = 0; i < envc; i++)
		envpp[i] = push_str(s, envp[i]);

	/* AT_RANDOM: reuse the 16 bytes the kernel gave us */
	uint8_t rnd[16];
	void *krandom = (void *)getauxval(AT_RANDOM);
	if (krandom)
		memcpy(rnd, krandom, 16);
	else
		for (int i = 0; i < 16; i++)
			rnd[i] = (uint8_t)(i * 7 + 1);
	uintptr_t at_random = push_bytes(s, rnd, 16);
	uintptr_t at_platform = push_str(s, "x86_64");
	uintptr_t at_execfn = push_str(s, exec_path);

	/* assemble the auxv */
	struct {
		uint64_t type, val;
	} aux[32];
	int n = 0;
#define AUX(t, v)                                                              \
	do {                                                                   \
		aux[n].type = (t);                                             \
		aux[n].val = (uint64_t)(v);                                    \
		n++;                                                           \
	} while (0)
	AUX(AT_PHDR, exe->phdr);
	AUX(AT_PHENT, exe->phent);
	AUX(AT_PHNUM, exe->phnum);
	AUX(AT_PAGESZ, PAGE);
	AUX(AT_BASE, have_interp ? interp->load_bias : 0);
	AUX(AT_FLAGS, 0);
	AUX(AT_ENTRY, exe->entry);
	AUX(AT_UID, getuid());
	AUX(AT_EUID, geteuid());
	AUX(AT_GID, getgid());
	AUX(AT_EGID, getegid());
	AUX(AT_SECURE, 0);
	AUX(AT_CLKTCK, getauxval(AT_CLKTCK));
	AUX(AT_HWCAP, getauxval(AT_HWCAP));
	AUX(AT_HWCAP2, getauxval(AT_HWCAP2));
	AUX(AT_RANDOM, at_random);
	AUX(AT_PLATFORM, at_platform);
	AUX(AT_EXECFN, at_execfn);
	unsigned long sysinfo = getauxval(AT_SYSINFO_EHDR); /* vDSO */
	if (sysinfo)
		AUX(AT_SYSINFO_EHDR, sysinfo);
	AUX(AT_NULL, 0);
#undef AUX

	/* Now lay the vector below the strings. Total words:
	 * argc(1) + argv(argc) + NULL(1) + envp(envc) + NULL(1) + 2*n. The
	 * stack pointer (at argc) must be 16-byte aligned. */
	size_t words = 1 + argc + 1 + envc + 1 + (size_t)2 * n;
	uintptr_t sp = (uintptr_t)s->p;
	sp -= words * 8;
	sp &= ~(uintptr_t)15; /* 16-align the argc slot */

	uint64_t *v = (uint64_t *)sp;
	size_t k = 0;
	v[k++] = argc;
	for (int i = 0; i < argc; i++)
		v[k++] = argp[i];
	v[k++] = 0;
	for (int i = 0; i < envc; i++)
		v[k++] = envpp[i];
	v[k++] = 0;
	for (int i = 0; i < n; i++) {
		v[k++] = aux[i].type;
		v[k++] = aux[i].val;
	}
	free(argp);
	free(envpp);
	return sp;
}

/* Set %rsp and jump. rdx must be 0 (no rtld_fini from us). No return. */
static __attribute__((noreturn)) void enter(uintptr_t sp, uintptr_t entry) {
	__asm__ volatile(
	    "mov %0, %%rsp\n\t"
	    "xor %%rdx, %%rdx\n\t"
	    "xor %%rbp, %%rbp\n\t"
	    "jmp *%1\n\t"
	    :
	    : "r"(sp), "r"(entry)
	    : "memory");
	__builtin_unreachable();
}

int run_native(sqlite3 *db, const char *path, char **argv, char **envp) {
	int64_t em = self_meta_int(db, "em");
	if (em != EM_X86_64)
		die("native mode supports x86-64 only (machine em=%ld)", em);

	struct object exe = {0}, interp = {0};
	map_program(db, &exe);

	char *interp_path = self_meta_text(db, "interp");
	int have_interp = interp_path != NULL;
	if (have_interp)
		map_interp(interp_path, &interp);

	int argc = 0;
	while (argv[argc])
		argc++;

	size_t stack_size = 8UL << 20;
	uint8_t *stk = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stk == MAP_FAILED)
		die("mmap stack: %m");
	struct stackbuf sb = {.base = stk, .size = stack_size,
			      .top = stk + stack_size, .p = stk + stack_size};

	uintptr_t sp = build_stack(&sb, argc, argv, envp, &exe, &interp,
				   have_interp, path);
	uintptr_t entry = have_interp ? interp.entry : exe.entry;

	sqlite3_close(db);
	free(interp_path);
	enter(sp, entry);
}
