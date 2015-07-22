#ifndef __vlvm_h
#define __vlvm_h

#include "vlvm_cfg.h"

typedef unsigned short vlty;
typedef uintptr_t vlreg;

#define NIL 0

typedef struct __vlproc {
    uintptr_t tyref;
    uintptr_t *stptr;
    uintptr_t regs[VLVM_NREGS];
} vlproc;

int vlty_init(vlty id, const char *name , size_t,
	      void (*)(void*,void*), void (*)(void*));
void vlty_gcsn(vlty id, void (*)(vlproc *,void *));
const char *vlty_name(vlty id);
const int vlty_imm(vlty id);

void vlgc_start(void);
void vlgc_run(vlproc *p);
void vlgc_end(void);
void vlgc_trim(void);

void vlvm_init(vlproc *proc);
void vlvm_gcwk(vlproc *proc, uintptr_t val);

void vlvm_new(vlproc *proc, vlreg reg, vlty type, void *arg);
void vlvm_set(vlproc *proc, vlreg reg, vlty type, void *ptr);
void vlvm_get(vlproc *proc, vlreg reg, vlty *type, void **ptr);
void vlvm_cpy(vlproc *proc, vlreg dst, vlreg src);

void vlvm_psh(vlproc *proc, vlreg reg);
int vlvm_pop(vlproc *proc, vlreg reg);

#endif
