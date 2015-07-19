#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include "vlvm.h"
#include "slab.h"

struct cons {
	uintptr_t a, d;
};

void cons_gcsn(vlproc *proc, void *ptr)
{
	struct cons *cons = (struct cons *)ptr;

	vlvm_gcwk(proc, cons->a);
	vlvm_gcwk(proc, cons->d);
}

vlproc vm;

void cons(vlreg dst, vlreg car, vlreg cdr)
{
	struct cons *cons;
	void *aptr, *dptr;
	vlty aty, dty;
	vlvm_un(&vm, car, &aty, &aptr);
	vlvm_un(&vm, cdr, &dty, &dptr);
	vlvm_en(&vm, dst, 1);
	vlvm_un(&vm, dst, NULL, (void **)&cons);

	cons->a = ((uintptr_t)aptr)|aty;
	cons->d = ((uintptr_t)dptr)|dty;
}

void tygcon() {}

int
main()
{
	int i;
	vlty ty;
	void *ptr;

	vlty_init(0, "NIL", 0); /* NIL TYPE */
	vlty_init(1, "CONS", sizeof(struct cons));
	vlty_gcsn(1, cons_gcsn);

	vlvm_init(&vm);
	for (i = 0; i < 1000; i++) {
		cons(2, 1, 2);
	}
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_un(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}

	tycache_dumpstats();
	vlvm_gc(&vm);
	tycache_dumpstats();

	vlvm_pu(&vm, 2);
	vlvm_en(&vm, 2, 0);

	printf("Push, cleared");
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_un(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}
	

	tycache_dumpstats();
	vlvm_gc(&vm);
	tycache_dumpstats();
	
	printf("pop: %d", vlvm_po(&vm, 0));
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_un(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}
	
	vlvm_en(&vm, 0, 0);
	printf("Cleared");
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_un(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}

	tycache_dumpstats();
	vlvm_gc(&vm);
	tycache_dumpstats();
	vlvm_trim();
	tycache_dumpstats();
	
	return 0;
}
