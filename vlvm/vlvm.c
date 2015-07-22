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

static void stgcscan(vlproc *proc);
void vlvm_gcwk(vlproc *proc, uintptr_t val)
{
	vlty ty = val & TYMASK;
	void *ptr = (void *)(val & ~TYMASK);
	struct type *type = types + ty;

	/* We do not support, rather arbitrarily, immediate root
	   objects.  We could, but not being able to mark them means
	   they'll be scanned multiple times. Enable them only if you 
	   need them. */
	if (type->imm)
		return;

	if (tycache_mark(ptr))
		return;

	if (type->gcscan != NULL)
		type->gcscan(proc, ptr);
}

static uintptr_t tyalloc(vlty type, void *arg)
{
	uintptr_t ret;
	struct type *ty;
	
	type = type < NTYPES ? type : 0;
	ty = types + type;

	if (ty->imm) return type;

	ret = (uintptr_t)tycache_alloc_opq(&ty->cache, arg);
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
vlty_init(vlty id, const char *name, size_t size,
	void (*ctr)(void *ptr,void*arg),
	void (*dtr)(void *ptr))
{
	int rc;

	if (id >= NTYPES)
		return -1;

	/* Type 0 is special: must be IMM (see tyalloc) and NIL is type 0 */
	if (!id && size)
		return -1;
	
	if (size) {
		rc = tycache_register(&types[id].cache,
				      name, size, ctr, dtr, TYPEBITS);
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
vlgc_start(void)
{
	GC_START();
	tycache_gcstart();
}

void
vlgc_run(vlproc *p)
{
	int i;

	for (i = 0; i < NREGS; i++)
		vlvm_gcwk(p, p->regs[i]);
	stgcscan(p);
}

void
vlgc_end(void)
{
	tycache_gcend();
	GC_END();
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
}

void
vlvm_new(vlproc *proc, vlreg reg, vlty type, void *arg)
{
	uintptr_t p;

	if ((reg >= NREGS) || (type >= NTYPES))
		return;
	GC_DISABLED();
	rgclear(proc, reg);
	p = tyalloc(type, arg);
	rgset(proc, reg, p);
	GC_ENABLED();
}

void
vlvm_cpy(vlproc *proc, vlreg dst, vlreg src)
{
	uintptr_t v = rgget(proc, src);

	GC_DISABLED();
	rgset(proc, dst, v);
	GC_ENABLED();
}

void
vlvm_get(vlproc *proc, vlreg reg, vlty *type, void **ptr)
{
	uintptr_t v;

	v = rgget(proc, reg);
	if (type)
		*type = v & TYMASK;
	if (ptr)
		*ptr = (void *)(v & ~TYMASK);
}

void
vlvm_set(vlproc *proc, vlreg reg, vlty type, void *ptr)
{
	uintptr_t v = type | (uintptr_t)ptr;

	GC_DISABLED();
	rgset(proc, v, reg);
	GC_ENABLED();
}

void
vlvm_psh(vlproc *proc, vlreg reg)
{
	uintptr_t v;

	v = rgget(proc, reg);
	GC_DISABLED();
	stgrow(proc);
	stset(proc, v);
	GC_ENABLED();
}

int
vlvm_pop(vlproc *proc, vlreg reg)
{
	int r;

	GC_DISABLED();
	rgset(proc, reg, stget(proc));
	r = stshrink(proc);	
	GC_ENABLED();

	return r;
}
