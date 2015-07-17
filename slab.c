#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/queue.h>

#define MAGIC_SIZE 16
#define MAGIC 0xdead5ade

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

	return (___slabsize() - sizeof(struct slabhdr)) / size;
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
	if (sh->magic != MAGIC)
		return NULL;
	return sh;
}

static void ___slabfree(void *ptr)
{

	munmap(ptr, ___slabsize());
}


/*
 * Generic, simple and portable slab allocator.
 */

#include "slab.h"

static int initialised = 0;
static size_t slab_size = 0;
static const uint64_t slab_magic = MAGIC;
static LIST_HEAD(slabqueue, slab) SLABSQUEUE;
static unsigned slabs = 0;
DECLARE_SPIN_LOCK(slabs_lock);

struct objhdr {
	SLIST_ENTRY(objhdr) list_entry;
};

int SLABFUNC(grow) (struct slab * sc)
{
	int i;
	struct objhdr *ptr;
	struct slabhdr *sh;
	const size_t objs = ___slabobjs(sc->objsize);
#define MAX(_x,_y) (((_x) > (_y)) ? (_x) : (_y))

	sh = ___slaballoc();;
	ptr = (struct objhdr *)((uintptr_t)sh +
				MAX(sizeof(struct slabhdr), sc->objsize));
	if (sh == NULL)
		return 0;

	sh->magic = slab_magic;
	sh->cache = sc;
	sh->freecnt = objs;
	SLIST_INIT(&sh->freeq);


	
	for (i = 0; i < objs; i++) {
		SLIST_INSERT_HEAD(&sh->freeq, ptr, list_entry);
		ptr = (struct objhdr *) ((uint8_t *) ptr + sc->objsize);
	}

	SPIN_LOCK(sc->lock);
	LIST_INSERT_HEAD(&sc->emptyq, sh, list_entry);
	sc->emptycnt++;
	SPIN_UNLOCK(sc->lock);

	return 1;
#undef MAX
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

	if (!sh && !LIST_EMPTY(&sc->emptyq)) {
		sh = LIST_FIRST(&sc->emptyq);
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

	addr = (void *) oh;
	memset(addr, 0, sizeof(*oh));

	if (sc->ctr)
		sc->ctr(addr, opq, 0);

      out:
	return addr;
}

void SLABFUNC(free) (void *ptr) {
	struct slab *sc;
	struct slabhdr *sh;
	unsigned max_objs;

	sh = ___slabgethdr(ptr);
	if (!sh)
		return;
	sc = sh->cache;
	max_objs = (slab_size - sizeof(struct slabhdr)) / sc->objsize;

	if (sc->ctr)
		sc->ctr(ptr, NULL, 1);

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

int
SLABFUNC(register) (struct slab * sc, const char *name, size_t objsize,
		    void (*ctr) (void *, void *, int), int align) {

	if (initialised == 0) {
		slab_size = ___slabsize();
		LIST_INIT(&SLABSQUEUE);
		SPIN_LOCK_INIT(slabs_lock);
		initialised++;
	}

	sc->name = (char *)name;

	if (align) {
		align = ((1L << align) - 1);
		sc->objsize = ((objsize + align) & ~align);
	} else {
		sc->objsize = objsize;
	}
	printf("objsize: %zd (%zd)\n", sc->objsize, objsize);
	sc->ctr = ctr;

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
	       " usage statistics:\n\t%-20s  %-8s\t%-9s\t%-8s\n", "Name",
	       "Empty", "Partial", "Full");
	SPIN_LOCK(slabs_lock);
	LIST_FOREACH(sc, &SLABSQUEUE, list_entry) {
		printf("\t%-20s  %-8d\t%-8d\t%-8d\n",
		       sc->name, sc->emptycnt, sc->freecnt, sc->fullcnt);
	}
	SPIN_UNLOCK(slabs_lock);
}