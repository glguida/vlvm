#ifndef __slab_h
#define __slab_h

#include <sys/queue.h>
#include "vlvm_cfg.h"

#define SLABFUNC_NAME "types cache"
#define SLABFUNC(_s) tycache_##_s
#define SLABSQUEUE slabsq

struct slab {
        DECLARE_SPIN_LOCK(lock);
	char *name;
	size_t objsize;
	void (*ctr) (void *obj, void *opq);
	void (*dtr) (void *obj);
	unsigned emptycnt;
	unsigned sweepcnt;
	unsigned freecnt;
	unsigned fullcnt;
	LIST_HEAD(, slabhdr) emptyq;
	LIST_HEAD(, slabhdr) sweepq;
	LIST_HEAD(, slabhdr) freeq;
	LIST_HEAD(, slabhdr) fullq;

	LIST_ENTRY(slab) list_entry;
};


int SLABFUNC(grow)(struct slab *sc);
int SLABFUNC(shrink) (struct slab * sc);
void *SLABFUNC(alloc_opq) (struct slab * sc, void *opq);
void SLABFUNC(free) (void *ptr);
void SLABFUNC(gcstart)(void);
int SLABFUNC(mark)(void *ptr);
void SLABFUNC(gcend)(void);
struct slab *SLABFUNC(resolve)(void *ptr);
int SLABFUNC(register) (struct slab * sc, const char *name, size_t objsize,
			void (*ctr) (void *, void *),
			void (*dtr) (void *),
			unsigned align);
void SLABFUNC(deregister) (struct slab * sc);
void SLABFUNC(dumpstats)(void);

#endif
