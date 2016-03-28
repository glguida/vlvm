#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "queue.h"

#define MAGIC_SIZE 16
#define MAGIC1 0xbad51ab
#define MAGIC2 0x600d51ab


#include "slab.h"
#undef MAX
#define MAX(_x,_y) (((_x) > (_y)) ? (_x) : (_y))

struct slab;
struct objhdr;

struct slabhdr {
	union {
		struct {
			uint64_t magic;
			struct slab *cache;
			unsigned freecnt;
			SLIST_HEAD(, objhdr) freeq;
			LIST_ENTRY(slabhdr) list_entry;
			uint64_t bmap[0];
		};
		char cacheline[64];
	};
};

/*
 * Implementation-dependent code.
 */

#include <sys/mman.h>
#include <unistd.h>

static const size_t ___slabsize(void)
{
	static size_t pgsz = 0;

	if (pgsz == 0)
		pgsz = (size_t)getpagesize();
	return pgsz;
}

static const size_t ___slabobjs(const size_t size)
{
	static size_t c1 = 0;
	if (c1 == 0)
		c1 = 8 * (___slabsize() - sizeof(struct slabhdr)) + 63;
	/*
	  b = (o + 63)/8;
	  o = (P - h - b)/s;
	  o = (P - h - (o + 63)/8)/s;
	  s * o = P - h - (o + 63)/8;
	  8s * o = 8P - 8h - o + 63;
	  o = (8P - 8h + 63)/(8s + 1)
	*/
	return c1 / (8 * size + 1) - 1;
}

static unsigned ___slabbmapno(struct slab *sc)
{

	return (___slabobjs(sc->objsize) + 63) / 64;
}

static size_t ___slabbmapsz(struct slab *sc)
{
	return sizeof(uint64_t)*___slabbmapno(sc);
}

static struct slabhdr *___slaballoc(void)
{
	void *ptr;

	ptr = mmap(NULL, ___slabsize(),
		   PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);
	if (ptr == NULL)
		return NULL;

	return (struct slabhdr *)ptr;
}

static struct slabhdr *___slabgethdr(void *obj)
{
	struct slabhdr *sh;
	uintptr_t addr = (uintptr_t) obj;

	sh = (struct slabhdr *) (addr & ~((uintptr_t) ___slabsize() - 1));
	if (sh->magic != MAGIC1 && sh->magic != MAGIC2)
		return NULL;
	return sh;
}

static uintptr_t ___slabgetptr(struct slabhdr *sh, unsigned objno)
{
	uintptr_t ptr;
	struct slab *sc = sh->cache;

	ptr = (uintptr_t)sh
		+ MAX(sizeof(struct slabhdr) + ___slabbmapsz(sc), sc->objsize);
	ptr += sc->objsize * objno;
	return ptr;
}

static unsigned ___slabgetobjno(struct slabhdr *sh, void *ptr)
{
	uintptr_t base = (uintptr_t)ptr;
	struct slab *sc = sh->cache;

	base -= (uintptr_t)sh
		+ MAX(sizeof(struct slabhdr) + ___slabbmapsz(sc), sc->objsize);
	return (unsigned)base / sc->objsize;
}

static void ___slabfree(void *ptr)
{

	munmap(ptr, ___slabsize());
}


/*
 * Generic, simple and portable slab allocator.
 */

static int initialised = 0;
static size_t slab_size = 0;
static LIST_HEAD(slabqueue, slab) SLABSQUEUE;
static unsigned slabs = 0;
DECLARE_SPIN_LOCK(slabs_lock);

struct objhdr {
	SLIST_ENTRY(objhdr) list_entry;
};

int SLABFUNC(grow) (struct slab * sc)
{
	struct slabhdr *sh;

	sh = ___slaballoc();
	___log("allocating slab %p\n", sh);
	if (sh == NULL)
		return 0;

	sh->magic = MAGIC1;
	sh->cache = sc;

	SPIN_LOCK(sc->lock);
	LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
	sc->emptycnt++;
	SPIN_UNLOCK(sc->lock);

	return 1;
}

int SLABFUNC(shrink) (struct slab * sc)
{
	int shrunk = 0;
	struct slabhdr *sh;

	SPIN_LOCK(sc->lock);
	while (!LIST_EMPTY(&sc->emptyq)) {
		sh = LIST_FIRST(&sc->emptyq);
		LIST_REMOVE(sh, list_entry);
		sc->emptycnt--;

		SPIN_UNLOCK(sc->lock);
		___slabfree((void *) sh);
		shrunk++;
		SPIN_LOCK(sc->lock);
	}
	SPIN_UNLOCK(sc->lock);

	return shrunk;
}

#define BPOS(i,j,k,l,m,h) (i * 64 + j * 32 + k * 16 + l * 8 + m * 4 + h)
static inline void
_free(struct slabhdr *sh,
      int i, int j, int k, int l, int m, int h)
{
	struct slab *sc = sh->cache;
	void *ptr;
	unsigned x = BPOS(i,j,k,l,m,h);
	if (x >= ___slabobjs(sh->cache->objsize))
		return;
	ptr = (void *)___slabgetptr(sh, x);

	if (sc->dtr)
		sc->dtr(ptr + MAX(sizeof(struct objhdr),
				  (1 << VLVM_TYPEBITS)));

	___dbg("Adding to list %p (%p:%d)\n", ptr, sh, x);
	SLIST_INSERT_HEAD(&sh->freeq, (struct objhdr *) ptr, list_entry);
}
#define FREE(_i,_j,_k,_l,_m,_h) _free(sh, (_i), (_j), (_k), (_l), (_m), (_h))

static inline void
bitscan(struct slabhdr *sh)
{
	uint64_t *ptr, val;
	int h,i,j,k,l, m;

	ptr = sh->bmap;
	for (i = 0; i < ___slabbmapno(sh->cache);  i++, ptr++) {
		val = *ptr;
		if (val == (uint64_t)-1)
			continue;
		if (val == 0) {
			___dbg("F64\n");
			for (h = 0; h < 64; h++)
				FREE(i,0,0,0,0,h);
			continue;
		}
		for (j = 0; j < 2; j++, val >>= 32) {
		    if ((val & 0xffffffff) == (uint32_t)-1)
				continue;
		    if ((val & 0xffffffff) == 0) {
			  ___dbg("F32\n");
				for (h = 0; h < 32; h++)
					FREE(i,j,0,0,0,h);
				continue;
			}
			for (k = 0; k < 2; k++, val >>= 16) {
			    if ((val & 0xffff) == (uint16_t)-1)
					continue;
			    if ((val & 0xffff) == 0) {
				  ___dbg("F16\n");
					for (h = 0; h < 16; h++)
						FREE(i,j,k,0,0,h);
					continue;
				}
				for (l = 0; l < 2; l++, val >>= 8) {
					if ((val & 0xff) == (uint8_t)-1)
						continue;
					if ((val & 0xff) == 0) {
					  ___dbg("F8\n");
						for (h = 0; h < 8; h++)
							FREE(i,j,k,l,0,h);
						continue;
					}
					for (m = 0; m < 2; m++, val >>= 4) {
						___dbg("F%lx\n", val & 0xf);
						switch(~val & 0xf) {
						case 0:
							break;
						case 15:
							FREE(i,j,k,l,m,3);
						case 7:
							FREE(i,j,k,l,m,2);
						case 3:
							FREE(i,j,k,l,m,1);
						case 1:
							FREE(i,j,k,l,m,0);
							break;
						case 14:
							FREE(i,j,k,l,m,3);
						case 6:
							FREE(i,j,k,l,m,2);
						case 2:
							FREE(i,j,k,l,m,1);
							break;
						case 13:
							FREE(i,j,k,l,m,3);
						case 5:
							FREE(i,j,k,l,m,0);
						case 4:
							FREE(i,j,k,l,m,2);
							break;
						case 9:
							FREE(i,j,k,l,m,0);
						case 8:
							FREE(i,j,k,l,m,3);
							break;
						case 11:
							FREE(i,j,k,l,m,0);
						case 10:
							FREE(i,j,k,l,m,1);
							FREE(i,j,k,l,m,3);
							break;
						case 12:
							FREE(i,j,k,l,m,2);
							FREE(i,j,k,l,m,3);
							break;
						}
					}
				}
			}
		}
	}
}


void *SLABFUNC(alloc_opq) (struct slab * sc, void *opq) {
	int tries = 0;
	void *addr = NULL;
	struct objhdr *oh;
	struct slabhdr *sh = NULL;


	SPIN_LOCK(sc->lock);

      retry:
	if (!LIST_EMPTY(&sc->freeq)) {
		sh = LIST_FIRST(&sc->freeq);
		if (sh->freecnt == 1) {
			LIST_REMOVE(sh, list_entry);
			LIST_INSERT_HEAD(&sc->fullq, sh, list_entry);
			sc->freecnt--;
			sc->fullcnt++;
		}
	}

	if (!sh && !LIST_EMPTY(&sc->sweepq)) {
		sh = LIST_FIRST(&sc->sweepq);
		bitscan(sh);
		sc->sweepcnt--;
		LIST_REMOVE(sh, list_entry);
		if (sh->freecnt == 1) {
			LIST_INSERT_HEAD(&sc->fullq, sh, list_entry);
			sc->fullcnt++;
		} else {
			LIST_INSERT_HEAD(&sc->freeq, sh, list_entry);
			sc->freecnt++;
		}
	}

	if (!sh && !LIST_EMPTY(&sc->emptyq)) {
		int i;
		uintptr_t ptr;
		const size_t objs = ___slabobjs(sc->objsize);

		sh = LIST_FIRST(&sc->emptyq);
		SLIST_INIT(&sh->freeq);
		ptr = ___slabgetptr(sh, 0);
		for (i = 0; i < objs; i++, ptr += sc->objsize)
			SLIST_INSERT_HEAD(&sh->freeq,
					  (struct objhdr *)ptr, list_entry);
		sh->freecnt = objs;
		LIST_REMOVE(sh, list_entry);
		LIST_INSERT_HEAD(&sc->freeq, sh, list_entry);
		sc->emptycnt--;
		sc->freecnt++;
	}

	if (!sh) {
		SPIN_UNLOCK(sc->lock);

		if (tries++ > 3)
			goto out;

		SLABFUNC(grow) (sc);

		SPIN_LOCK(sc->lock);
		goto retry;
	}

	oh = SLIST_FIRST(&sh->freeq);
	SLIST_REMOVE_HEAD(&sh->freeq, list_entry);
	sh->freecnt--;
	SPIN_UNLOCK(sc->lock);


	addr = (void *)oh;
	memset(addr, 0, sc->objsize);
	if (sc->dtr)
		addr += MAX(sizeof(struct objhdr), (1 << VLVM_TYPEBITS));
	if (sc->ctr)
		sc->ctr(addr, opq);

      out:
	return addr;
}

void SLABFUNC(free) (void *ptr) {
	struct slab *sc;
	struct slabhdr *sh;
	unsigned max_objs;

	sh = (struct slabhdr *)___slabgethdr(ptr);
	if (!sh)
		return;
	sc = sh->cache;
	max_objs = ___slabobjs(sc->objsize);

	if (sc->dtr)
		sc->dtr(ptr + MAX(sizeof(struct objhdr),(1 << VLVM_TYPEBITS)));

	SPIN_LOCK(sc->lock);
	SLIST_INSERT_HEAD(&sh->freeq, (struct objhdr *) ptr, list_entry);
	sh->freecnt++;

	if (sh->freecnt == 1) {
		LIST_REMOVE(sh, list_entry);
		LIST_INSERT_HEAD(&sc->freeq, sh, list_entry);
		sc->fullcnt--;
		sc->freecnt++;
	} else if (sh->freecnt == max_objs) {
		LIST_REMOVE(sh, list_entry);
		LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
		sc->freecnt--;
		sc->emptycnt++;
	}
	SPIN_UNLOCK(sc->lock);

	return;
}

struct slab *SLABFUNC(resolve)(void *ptr)
{
	struct slabhdr *sh;

	sh = ___slabgethdr(ptr);
	return sh ? sh->cache : NULL;
}

void SLABFUNC(gcstart)(void)
{
	struct slab *sc;
	struct slabhdr *sh, *tvar;

	SPIN_LOCK(slabs_lock);
	LIST_FOREACH(sc, &SLABSQUEUE, list_entry) {
		SPIN_LOCK(sc->lock);
		LIST_FOREACH_SAFE(sh, &sc->sweepq, list_entry, tvar) {
			sh->magic = MAGIC2;
			sh->freecnt = ___slabobjs(sc->objsize);
			memset(sh->bmap, 0, ___slabbmapsz(sc));
			LIST_REMOVE(sh, list_entry);
			LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
			sc->sweepcnt--;
			sc->emptycnt++;
		}
		LIST_FOREACH_SAFE(sh, &sc->freeq, list_entry, tvar) {
			sh->magic = MAGIC2;
			sh->freecnt = ___slabobjs(sc->objsize);
			memset(sh->bmap, 0, ___slabbmapsz(sc));
			LIST_REMOVE(sh, list_entry);
			LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
			sc->freecnt--;
			sc->emptycnt++;
		}
		LIST_FOREACH_SAFE(sh, &sc->fullq, list_entry, tvar) {
			sh->magic = MAGIC2;
			sh->freecnt = ___slabobjs(sc->objsize);
			memset(sh->bmap, 0, ___slabbmapsz(sc));
			LIST_REMOVE(sh, list_entry);
			LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
			sc->fullcnt--;
			sc->emptycnt++;
		}
		SPIN_UNLOCK(sc->lock);
	}
	SPIN_UNLOCK(slabs_lock);
}

int
SLABFUNC(mark)(void *ptr)
{
	int marked;
	struct slab *sc;
	struct slabhdr *sh;
	unsigned objno, bitno;

	sh = ___slabgethdr(ptr);
	sc = sh->cache;
	objno = ___slabgetobjno(sh, ptr) / 64;
	bitno = ___slabgetobjno(sh, ptr) % 64;

	SPIN_LOCK(sc->lock);
	if (sh->magic == MAGIC2) {
		LIST_REMOVE(sh, list_entry);
		LIST_INSERT_HEAD(&sc->sweepq, sh, list_entry);
		sc->emptycnt--;
		sc->sweepcnt++;
		sh->magic = MAGIC1;
		SLIST_INIT(&sh->freeq);	
	}

	marked = sh->bmap[objno] & (1LL << bitno);
	if (marked)
		goto out;

	sh->bmap[objno] |= (1LL << bitno);
	sh->freecnt--;
	if (sh->freecnt == 0) {
		LIST_REMOVE(sh, list_entry);
		LIST_INSERT_HEAD(&sc->fullq, sh, list_entry);
		sc->sweepcnt--;
		sc->fullcnt++;
	}
	SPIN_UNLOCK(sc->lock);

out:
	return marked;
}

void
SLABFUNC(gcend) (void)
{
	int i;
	struct slab *sc;
	struct slabhdr *sh;

	SPIN_LOCK(slabs_lock);
	LIST_FOREACH(sc, &SLABSQUEUE, list_entry) {
		if (sc->dtr == NULL)
			continue;
		SPIN_LOCK(sc->lock);
		LIST_FOREACH(sh, &sc->emptyq, list_entry) {
			uintptr_t ptr;
			const size_t objs = ___slabobjs(sc->objsize);

			for (i = 0; i < objs; i++) {
				ptr = ___slabgetptr(sh, i);
				sc->dtr((void *)ptr
					+ MAX(sizeof(struct objhdr),
					      (1 << VLVM_TYPEBITS)));
			}
		}
		SPIN_UNLOCK(sc->lock);
	}
	SPIN_UNLOCK(slabs_lock);
}

int
SLABFUNC(register) (struct slab * sc, const char *name, size_t objsize,
		    void (*ctr) (void *, void *), void (*dtr) (void *),
		    unsigned align) {

	if (initialised == 0) {
		slab_size = ___slabsize();
		LIST_INIT(&SLABSQUEUE);
		SPIN_LOCK_INIT(slabs_lock);
		initialised++;
	}

	sc->name = (char *)name;

	/* Add objhdr to size of object in case of destructor,
         * as we can't differenziate entries in the free list and
         * freed pointers through garbage collection */
	if (dtr)
		objsize += MAX(sizeof(struct objhdr), (1 << VLVM_TYPEBITS));
	if (align) {
		align = ((1L << align) - 1);
		sc->objsize = ((objsize + align) & ~align);
	} else {
		sc->objsize = objsize;
	}
	printf("objsize: %zd (%zd)\n", sc->objsize, objsize);
	sc->ctr = ctr;
	sc->dtr = dtr;

	sc->emptycnt = 0;
	sc->freecnt = 0;
	sc->fullcnt = 0;

	SPIN_LOCK_INIT(sc->lock);
	LIST_INIT(&sc->emptyq);
	LIST_INIT(&sc->freeq);
	LIST_INIT(&sc->fullq);

	SPIN_LOCK(slabs_lock);
	LIST_INSERT_HEAD(&SLABSQUEUE, sc, list_entry);
	slabs++;
	SPIN_UNLOCK(slabs_lock);

	return 0;
}

void SLABFUNC(deregister) (struct slab * sc) {
	struct slabhdr *sh;

	while (!LIST_EMPTY(&sc->emptyq)) {
		sh = LIST_FIRST(&sc->emptyq);
		LIST_REMOVE(sh, list_entry);

		___slabfree((void *) sh);
	}

	while (!LIST_EMPTY(&sc->sweepq)) {
		sh = LIST_FIRST(&sc->freeq);
		LIST_REMOVE(sh, list_entry);
		___slabfree((void *) sh);
	}

	while (!LIST_EMPTY(&sc->freeq)) {
		sh = LIST_FIRST(&sc->freeq);
		LIST_REMOVE(sh, list_entry);
		___slabfree((void *) sh);
	}

	while (!LIST_EMPTY(&sc->fullq)) {
		sh = LIST_FIRST(&sc->fullq);
		LIST_REMOVE(sh, list_entry);
		___slabfree((void *) sh);
	}

	SPIN_LOCK_FREE(sc->lock);

	SPIN_LOCK(slabs_lock);
	LIST_REMOVE(sc, list_entry);
	slabs--;
	SPIN_UNLOCK(slabs_lock);
}

void SLABFUNC(dumpstats) (void) {
	struct slab *sc;

	printf(SLABFUNC_NAME
	       " usage statistics:\n\t%-20s  %-8s\t%-9s\t%s\t%s\n", "Name",
	       "Empty", "To sweep", "Partial", "Full");
	SPIN_LOCK(slabs_lock);
	LIST_FOREACH(sc, &SLABSQUEUE, list_entry) {
		printf("\t%-20s  %-8d\t%-8d\t%-8d%-8d\n",
		       sc->name, sc->emptycnt, sc->sweepcnt, sc->freecnt, sc->fullcnt);
	}
	SPIN_UNLOCK(slabs_lock);
}
