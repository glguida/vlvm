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
const char *vlty_name(vlty id);

void vlvm_init(vlproc *proc);
void vlvm_en(vlproc *proc, vlreg op, vlty type);
void vlvm_cp(vlproc *proc, vlreg dst, vlreg src);
void vlvm_pu(vlproc *proc, vlreg op);
int vlvm_po(vlproc *proc, vlreg op);
void vlvm_un(vlproc *proc, vlreg op, vlty *type, void **ptr);

/* PROBLABLY IMPLEMENT THIS: 
 *  There are two differents kind of types. the one calling the slab allocator
 * and the immediate ones, which can be set in value as they please. */
void vlvm_im(vlreg reg, uintptr_t im); /* If type is IMM */

#endif
