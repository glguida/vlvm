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
 * VLVM machine dependent
 */

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

#endif

