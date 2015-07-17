#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include "vlvm.h"
#include "slab.h"

struct cons {
  uintptr_t a, d;
};

void tygcon() {}

int
main()
{
  int i;
  vlproc vm;

  vlty_init(0, "NIL", 0); /* NIL TYPE */
  vlty_init(1, "CONS", sizeof(struct cons));

  vlvm_init(&vm);
  for (i = 0; i < 2000; i++) {
    vlvm_pu(&vm, i % VLVM_NREGS);
    vlvm_en(&vm, i % VLVM_NREGS, 1);
  }

  tycache_dumpstats();
  for (i = 0; i < VLVM_NREGS; i++) {
    vlty ty;
    void *ptr;
    
    vlvm_un(&vm, i, &ty, &ptr);
    printf("REG%01X: %016p (%s)\n", i, ptr, vlty_name(ty));
  }
  for (i = 0; i < 2000; i++) {
    vlty ty;
    void *ptr;

    assert(!vlvm_po(&vm, 0));
    vlvm_un(&vm, 0, &ty, &ptr);
    printf("ST%03X: %016p (%s)\n", i, ptr, vlty_name(ty));

  }
  return 0;
}
