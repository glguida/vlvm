#include <inttypes.h>

#include "vlvm.h"
#include "slab.h"

#define NREGS VLVM_NREGS
#define TYPEBITS VLVM_TYPEBITS
#define NTYPES (1L << TYPEBITS)
#define TYMASK (NTYPES - 1)

struct type {
	char        *name;
	struct slab cache;
	unsigned    imm:1;
	void        (*gcscan)(vlproc *, void *);
};

struct sthdr {
	uint64_t magic;
	struct sthdr *prev;
};

static struct type types[NTYPES];
LIST_HEAD(vlprocs, __vlproc) vlvm_procs = LIST_HEAD_INITIALIZER(vlvm_procs);

static void stgcscan(vlproc *proc);

void vlvm_gcwk(vlproc *proc, uintptr_t val)
{
	vlty ty = val & TYMASK;
	void *ptr = (void *)(val & ~TYMASK);

	if (!types[ty].imm)
		tycache_mark(ptr);

	if (types[ty].gcscan != NULL)
		types[ty].gcscan(proc, ptr);
}

static uintptr_t tyalloc(vlty type)
{
	uintptr_t ret;
	struct type *ty;
	
	type = type < NTYPES ? type : 0;
	ty = types + type;

	if (ty->imm) return type;

	ret = (uintptr_t)tycache_alloc_opq(&ty->cache, NULL);
	ret |= type;
	return ret;
}

static uintptr_t
rgget(vlproc *proc, vlreg reg)
{

    if (reg >= NREGS)
		return NIL;
	return proc->regs[reg];
}

static void
rgclear(vlproc *proc, vlreg reg)
{

	if (reg >= NREGS)
		return;
	proc->regs[reg] = NIL;
}

static void
rgset(vlproc *proc, vlreg reg, uintptr_t val)
{

	if (reg >= NREGS)
		return;
	proc->regs[reg] = val;
}

static uintptr_t *
stfirst(struct sthdr *sthdr)
{
	uintptr_t ptr;

	ptr = (uintptr_t)sthdr + sizeof(struct sthdr);
	ptr = (ptr + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t)- 1);
	return (uintptr_t *)ptr;
}

static uintptr_t *
stlast(struct sthdr *sthdr)
{
	uintptr_t ptr;

	if (sthdr == NULL)
		return NULL;

	ptr =(uintptr_t)sthdr + ___pagesize() - sizeof(uintptr_t);
	return (uintptr_t *)ptr;
}

static struct sthdr *
sthdr(void *ptr)
{

	return (struct sthdr *)((uintptr_t)ptr & ___pagemask());
}

static void
stgrow(vlproc *proc)
{
	uintptr_t *ptr = proc->stptr + 1;

	if (proc->stptr == NULL || sthdr(proc->stptr) != sthdr(ptr)) {
		struct sthdr *new;

		new = (struct sthdr *)___allocpage();
		new->prev = sthdr(proc->stptr);
		new->magic = 0xcafebabe;
		ptr = stfirst(new);
	}
	proc->stptr = ptr;
}

static int
stshrink(vlproc *proc)
{
	if (proc->stptr == NULL)
		return -1;

	if (proc->stptr == stfirst(sthdr(proc->stptr))) {
		struct sthdr *prev, *curr;

		curr = sthdr(proc->stptr);
		prev = curr->prev;
		proc->stptr = stlast(prev);
		___freepage((void *)curr);
	} else {
		proc->stptr--;
	}
	return 0;
}

static void
stgcscan(vlproc *proc)
{
	uintptr_t *first, *stptr = proc->stptr;

	while (stptr != NULL) {
		first = stfirst(sthdr(stptr));
		do {
			vlvm_gcwk(proc, *stptr);
		} while (--stptr >= first);
		stptr = stlast(sthdr(stptr)->prev);
	}
}

static uintptr_t
stget(vlproc *proc)
{

	return *proc->stptr;
}

static void
stset(vlproc *proc, uintptr_t val)
{

	*proc->stptr = val;
}

int
vlty_init(vlty id, const char *name, size_t size)
{
	int rc;

	if (id >= NTYPES)
		return -1;

	/* Type 0 is special: must be IMM (see tyalloc) and NIL is type 0 */
	if (!id && size)
		return -1;
	
	if (size) {
		rc = tycache_register(&types[id].cache,
				      name, size, NULL, TYPEBITS);
		if (rc) return -1;
	}
	types[id].name = (char *)name;
	types[id].imm = (size == 0);
	types[id].gcscan = NULL;

	return 0;
}

void
vlty_gcsn(vlty id, void (*fn)(vlproc *, void *))
{

	types[id].gcscan = fn;
}

const char *
vlty_name(vlty id)
{
	if (id >= NTYPES)
		return NULL;

	return types[id].name;
}

const int
vlty_imm(vlty id)
{
	if (id >= NTYPES)
		return 0;

	return types[id].imm;
}

void
vlgc_run(void)
{
	int i;
	vlproc *p;

	tycache_gcstart();
	LIST_FOREACH(p, &vlvm_procs, vlist) {
		for (i = 0; i < NREGS; i++)
			vlvm_gcwk(p, p->regs[i]);
		stgcscan(p);
	}
	tycache_gcend();
}

void
vlgc_trim(void)
{
	vlty i;

	for (i = 0; i <NTYPES; i++)
		if (!types[i].imm)
			tycache_shrink(&types[i].cache);
}

void
vlvm_init(vlproc *proc)
{
	int i;

	proc->stptr = NULL;
	proc->tyref = NIL;
	for (i = 0; i < NREGS; i++)
		proc->regs[i] = NIL;

	LIST_INSERT_HEAD(&vlvm_procs, proc, vlist);
}

void
vlvm_en(vlproc *proc, vlreg reg, vlty type)
{
	uintptr_t p;

	if ((reg >= NREGS) || (type >= NTYPES))
		return;
	rgclear(proc, reg);
	p = tyalloc(type); /* gc disabled */
	rgset(proc, reg, p);
	p = NIL; tygcon(); /* gc reenabled */
}

void
vlvm_cp(vlproc *proc, vlreg dst, vlreg src)
{

	rgset(proc, dst, rgget(proc, src));
}

void
vlvm_pu(vlproc *proc, vlreg reg)
{

	stgrow(proc);
	stset(proc, rgget(proc, reg));
}

int
vlvm_po(vlproc *proc, vlreg reg)
{

	rgset(proc, reg, stget(proc));
	return stshrink(proc);
}

void
vlvm_un(vlproc *proc, vlreg reg, vlty *type, void **ptr)
{
	uintptr_t v;

	v = rgget(proc, reg);
	if (type)
		*type = v & TYMASK;
	if (ptr)
		*ptr = (void *)(v & ~TYMASK);
}
