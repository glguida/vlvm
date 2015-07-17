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
};

struct sthdr {
	struct sthdr *prev;
};


static struct type types[NTYPES];


static inline vlty tyid(struct type *ty)
{
	return ((uintptr_t)ty-(uintptr_t)types)/sizeof(types[0]);
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

	if (sthdr(proc->stptr) != sthdr(ptr)) {
		struct sthdr *new;

		new = (struct sthdr *)___allocpage();
		new->prev = sthdr(proc->stptr);
		ptr = stfirst(new);
	}
	proc->stptr = ptr;
}

static int
stshrink(vlproc *proc)
{
	if (proc->stptr == stfirst(sthdr(proc->stptr))) {
		struct sthdr *prev, *curr;

		curr = sthdr(proc->stptr);
		prev = curr->prev;
		if (prev == NULL)
			return -1;
		proc->stptr = stlast(prev);
		___freepage((void *)curr);
	} else {
		proc->stptr--;
	}
	return 0;
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

	return 0;
}

const char *
vlty_name(vlty id)
{
	if (id >= NTYPES)
		return NULL;

	return types[id].name;
}

void
vlvm_init(vlproc *proc)
{
	int i;
	struct sthdr *sh;

	sh = (struct sthdr *)___allocpage();
	sh->prev = NULL;
	proc->stptr = stfirst(sh);
	proc->tyref = NIL;
	printf("stptr = %p\n", proc->stptr);;
	for (i = 0; i < NREGS; i++)
		proc->regs[i] = NIL;
}

void
vlvm_en(vlproc *proc, vlreg reg, vlty type)
{
	uintptr_t p;

	if ((reg >= NREGS) || (type >= NTYPES))
		return;
	rgclear(proc, reg);
	p = tyalloc(type); /* gc disabled */
	printf("%p\n", p);
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
