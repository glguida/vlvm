#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <search.h>
#include <setjmp.h>
#include <inttypes.h>
#include "vlvm.h"
#include "vlisp.h"


#include "slab.h"

vlproc vm;

char *estr;
jmp_buf ebuf;

#define evlvm_error(...)			\
	do {					\
		asprintf(&estr, __VA_ARGS__);	\
		longjmp(ebuf,1);		\
	} while(0)

/*
 * Garbage collector.
 * (single threaded)
 */

void gcrun(void)
{
	vlgc_start();
	vlgc_run(&vm);
	vlgc_end();
	//	vlgc_trim();
}


/*
 * Types
 */

#define TYPE_NIL VLTY_NIL

#define TYPE_CONS VLTY_CONS
struct cons {
	uintptr_t a, d;
};

struct cons_arg {
	vlreg ar, dr;
};

void cons_ctr(void *ptr, void *arg)
{
	vlty aty, dty;
	uintptr_t aptr, dptr;
	struct cons *d = (struct cons *)ptr;
	struct cons_arg *s = (struct cons_arg *)arg;

	if (arg == NULL) {
		d->a = NIL;
		d->d = NIL;
		return;
	}

	vlvm_get(&vm, s->ar, &aty, (void **)&aptr);
	vlvm_get(&vm, s->dr, &dty, (void **)&dptr);
	d->a = aty | aptr;
	d->d = dty | dptr;
}

void cons_gcsn(vlproc *proc, void *ptr)
{
	struct cons *cons = (struct cons *)ptr;

	vlvm_gcwk(proc, cons->a);
	vlvm_gcwk(proc, cons->d);
}

#define TYPE_STRG VLTY_STRG
void str_ctr(void *ptr, void *arg)
{

	printf("CONSTRUCTING <%s> (%p)\n", (char *)arg, ptr);
	asprintf((char **)ptr, "%s", (char *)arg);
	printf("ptr is %p\n", ptr);
}

void str_dtr(void *ptr)
{
	char *s = *(char **)ptr;

	free(s);
	*(void **)ptr = NULL;
}

#define TYPE_ENV VLTY_ENV
#define ENV_SIZE 32

struct env_arg {
	vlreg base;
};

struct env {
	struct env_entry {
		uintptr_t key;
		uintptr_t val;
		struct env_entry *next;
	} *table[ENV_SIZE];
	struct env *next;
};

static void env_ctr(void *ptr, void *arg)
{
	vlty ty;
	uintptr_t regptr;
	struct env_arg *envarg = (struct env_arg *)arg;
	struct env *new = (struct env *)ptr;

	memset(new, 0, sizeof(*new));
	if (arg == NULL)
		return;

	vlvm_get(&vm, envarg->base, &ty, &regptr);
	if (ty != TYPE_ENV)
		return;
	*new = *(struct env *)regptr;
	new->next = (struct env *)regptr;
	return;
}

static void env_dtr(void *ptr)
{
	struct env *env = (struct env *)ptr;
	struct env_entry *e, *n, *l;
	int i;

	for (i = 0; i < ENV_SIZE; i++) {
		l = env->next ? env->next->table[i] : NULL;
		e = env->table[i];
		while (e != l) {
			n = e->next;
			free(e);
			e = n;
		}
	}
	memset(env, 0, sizeof(*env));
}

void
env_gcsn(vlproc *proc, void *ptr)
{
	int i;
	struct env *env = (struct env *)ptr;
	struct env_entry *e;

	for (i = 0; i < ENV_SIZE; i++) {
		e = env->table[i];
		while (e != NULL) {
			vlvm_gcwk(proc, e->key);
			vlvm_gcwk(proc, e->val);
			e = e->next;
		}
	}
	if (env->next) {
		vlvm_gcwk(proc, ((uintptr_t)env->next) | TYPE_ENV);
	}
}

static unsigned env_hashf(uintptr_t key)
{
	return (key >> 5) % ENV_SIZE;
}

uintptr_t
env_lookup(struct env *env, uintptr_t key)
{
	struct env_entry *e;
	unsigned n;

	n = env_hashf(key);	
	e = env->table[n];
	while (e != NULL) {
		if (e->key == key)
			break;
		e = e->next;
	}

	if (e == NULL)
		evlvm_error("Undefined %s", key);
	return e->val;
}

void
env_define(struct env *env, uintptr_t key, uintptr_t value)
{
	struct env_entry *e;
	unsigned n = env_hashf(key);

	e = malloc(sizeof(struct env_entry));
	e->key = key;
	e->val = value;
	e->next = env->table[n];
	env->table[n] = e;	/* This changes root */
}

void
env_set(struct env *env, uintptr_t key, uintptr_t value)
{
	unsigned n = env_hashf(key);
	struct env_entry *e;

	e = env->table[n];
	while (e != NULL) {
		if (e->key == key)
			break;
		e = e->next;
	}

	if (e == NULL)
		evlvm_error("Undefined", key);
	e->val = value; /* This changes root */
}

#undef MAX
#define MAX(_x,_y) (((_x) > (_y)) ? (_x) : (_y))
#define TYPE_SYMB VLTY_SYMB
uintptr_t
symbol(char *string)
{
	ENTRY e;
	char *sym;
	size_t len, aln;


	aln = MAX(1 << VLVM_TYPEBITS, sizeof(void *));
	len = strlen(string) + 1;
	posix_memalign((void **)&sym, aln, len);
	memcpy(sym, string, len);

	/* Intern the symbol. Not possible to deintern, don't GC */
	e.key = sym;
	sym = hsearch(e, ENTER)->key;
	if (sym != e.key)
		free(e.key);

	return ((uintptr_t)sym) | TYPE_SYMB;
}

/*
 * LVOPS
 */

/* CAR AND CDR SHOULD BE OPS. INTERNAL FUNCTIONS AND PROCEDURES */
void
car(vlreg dst, vlreg reg)
{
	struct cons *cons;
	uintptr_t r;
	vlty ty;

	vlvm_get(&vm, reg, &ty, (void **)&cons);
	if (ty == TYPE_NIL)
		r = NIL;
	if (ty == TYPE_CONS)
		r = cons->a;
	else
		evlvm_error("not a cons", ty);

	vlvm_new(&vm, dst, r, NULL);
}

void
cdr(vlreg dst, vlreg reg)
{
	struct cons *cons;
	uintptr_t r;
	vlty ty;

	vlvm_get(&vm, reg, &ty, (void **)&cons);
	if (ty == TYPE_NIL)
		r = NIL;
	if (ty == TYPE_CONS)
		r = cons->d;
        evlvm_error("not a cons", ty);

	vlvm_new(&vm, dst, r, NULL);
}

void
cons(vlreg dst, vlreg ar, vlreg dr)
{
	struct cons_arg arg;

	arg.ar = ar;
	arg.dr = dr;
	vlvm_new(&vm, dst, TYPE_CONS, &arg);
}

void eval(vlreg dst, vlreg exp, vlreg env)
{
	
}

int
main()
{
	int i;
	vlty ty;
	struct env *env1, *env2;
	struct env_arg env_arg;
	void *ptr;

	hcreate(1000);

	vlty_init(TYPE_NIL, "NIL", 0, NULL, NULL); /* NIL TYPE */
	vlty_init(TYPE_STRG, "STRING", sizeof(char *), str_ctr, str_dtr);
	vlty_init(TYPE_SYMB, "SYMBOL", 0, NULL, NULL);	
	vlty_init(TYPE_CONS, "CONS", sizeof(struct cons), cons_ctr, NULL);
	vlty_init(TYPE_ENV, "SYMTABLE", sizeof(struct env), env_ctr, env_dtr);

	vlty_gcsn(TYPE_CONS, cons_gcsn);
	vlty_gcsn(TYPE_ENV, env_gcsn);
	vlvm_init(&vm);

	if (setjmp(ebuf)) {
		free(estr);
		return -1;
	};
	
	for (i = 0; i < 1000; i++) {
		cons(2, 1, 2);
	}
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
	}

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	vlvm_psh(&vm, 2);
	vlvm_new(&vm, 2, 0, NULL);

	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
	}
	

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();
	
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
	}
	
	vlvm_new(&vm, 0, 0, NULL);
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
	}

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	vlvm_new(&vm, 0, TYPE_STRG, "Ciao Mondo");
	vlvm_cpy(&vm, 1, 0);
	cons(2,0,1);

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	vlvm_new(&vm, 0, TYPE_NIL, NULL);
	vlvm_new(&vm, 1, TYPE_NIL, NULL);	

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	vlvm_new(&vm, 2, TYPE_NIL, NULL);

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	printf("%lx ", symbol("asd"));
	printf("%lx ", symbol("asd"));
	printf("%lx ", symbol("asd"));
	printf("%lx ", symbol("ase"));
	printf("%lx ", symbol("asd"));

	printf("\t\t\tENV base at 3!\n");
	vlvm_new(&vm, 3, TYPE_ENV, NULL);
	tycache_dumpstats();

	vlvm_get(&vm, 3, NULL, (void **)&env1);
	env_define(env1, symbol("X"), symbol("Value of X"));
	env_define(env1, symbol("asd"), symbol("Value of asd"));
	printf("X: %lx\n", env_lookup(env1, symbol("X")));
	printf("Y: %lx\n", env_lookup(env1, symbol("asd")));

	printf("\t\t\tENV at 4 with base 3!\n");
	env_arg.base = 3;
	vlvm_new(&vm, 4, TYPE_ENV, &env_arg);
	env_arg.base = 4;
	vlvm_get(&vm, 4, NULL, (void **)&env2);
	printf("%p <->  %p\n", env1->table[5], env2->table[5]);
		
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("Y: %lx\n", env_lookup(env2, symbol("asd")));
	env_define(env2, symbol("X"), symbol("New symbol of X"));
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("X: %lx\n", env_lookup(env1, symbol("X")));	

	printf("\t\t\tREMOVE ENV 3\n");
	vlvm_new(&vm, 3, TYPE_NIL, NULL);
	printf("GCRUN!\n");
	gcrun();
	tycache_dumpstats();
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("X: %lx\n", env_lookup(env2, symbol("asd")));
	tycache_dumpstats();
	env_arg.base = 4;
	printf("\t\t\tENV 4 based on ENV4!\n");
	vlvm_new(&vm, 4, TYPE_ENV, &env_arg);
	tycache_dumpstats();
	printf("GCRUN!\n");
	gcrun();
	tycache_dumpstats();
	printf("LAST RUN!\n");
	vlvm_new(&vm, 4, TYPE_NIL, NULL);
	gcrun();
	tycache_dumpstats();
	
	return 0;
}
