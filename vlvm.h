#ifndef __vlvm_h
#define __vlvm_h

#include <inttypes.h>
#include <sys/queue.h>
#include "vlvm_cfg.h"


typedef unsigned short vlty;
typedef uintptr_t vlreg;

#define NIL 0

typedef struct __vlproc {
    LIST_ENTRY(__vlproc) vlist;
    uintptr_t tyref;
    uintptr_t *stptr;
    uintptr_t regs[VLVM_NREGS];
} vlproc;

int vlty_init(vlty id, const char *name, size_t size);
void vlty_gcsn(vlty id, void (*)(vlproc *,void *));
const char *vlty_name(vlty id);
const int vlty_imm(vlty id);

void vlgc_run(void);
void vlgc_trim(void);

void vlvm_init(vlproc *proc);
void vlvm_gcwk(vlproc *proc, uintptr_t val);

void vlvm_en(vlproc *proc, vlreg op, vlty type);
void vlvm_cp(vlproc *proc, vlreg dst, vlreg src);
void vlvm_pu(vlproc *proc, vlreg op);
int vlvm_po(vlproc *proc, vlreg op);
void vlvm_un(vlproc *proc, vlreg op, vlty *type, void **ptr);

#endif
