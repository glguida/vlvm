#ifndef __vlvm_cfg_h
#define __vlvm_cfg_h

/*
 * VLVM configuration.
 */

#ifndef VLVM_TYPEBITS
#define VLVM_TYPEBITS 6
#endif
#ifndef VLVM_NREGS
#define VLVM_NREGS 16
#endif


/*
 * VLVM machine dependent settings
 */

#include "queue.h"
#include <inttypes.h>
#include <sys/mman.h>
#include <unistd.h>

static inline const size_t ___pagesize(void)
{
	static size_t pgsz = 0;

	if (pgsz == 0)
		pgsz = (size_t)getpagesize();
	return pgsz;
}

static inline const uintptr_t ___pagemask(void)
{
	static uintptr_t mask = 0;

	if (mask == 0)
		mask = ~(uintptr_t)(___pagesize() - 1);
		
	return mask;
}

static inline void *___allocpage(void)
{
	void *ptr;

	ptr = mmap(NULL, ___pagesize(),
		   PROT_READ|PROT_WRITE, MAP_ANON|MAP_SHARED, -1, 0);
	return ptr;
}

static inline void ___freepage(void *ptr)
{

	munmap(ptr, ___pagesize());
}

#ifndef DECLARE_SPIN_LOCK
#define DECLARE_SPIN_LOCK(_x)
#endif
#ifndef SPIN_LOCK_INIT
#define SPIN_LOCK_INIT(_x)
#endif
#ifndef SPIN_LOCK
#define SPIN_LOCK(_x)
#endif
#ifndef SPIN_UNLOCK
#define SPIN_UNLOCK(_x)
#endif
#ifndef SPIN_LOCK_FREE
#define SPIN_LOCK_FREE(_x)
#endif

#ifndef GC_DISABLED
#define GC_DISABLED(_x)
#endif
#ifndef GC_ENABLED
#define GC_ENABLED(_x)
#endif
#ifndef GC_START
#define GC_START(_x)
#endif
#ifndef GC_END
#define GC_END(_x)
#endif


#define ___log(...) printf(__VA_ARGS__)
#define ___dbg(...) printf(__VA_ARGS__)

#ifndef ___dbg
#define ___dbg(...)
#endif

#ifndef ___log
#define ___log(...)
#endif

#endif

