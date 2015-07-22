#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <search.h>
#include <inttypes.h>
#include "vlvm.h"
#include "vlisp.h"


#include "slab.h"

vlproc vm;

#define evlvm_error(...) do { } while(0)

/*
 * Garbage collector.
 * (single threaded)
 */

void gcrun(void)
{
	vlgc_start();
	vlgc_run(&vm);
	vlgc_end();
	vlgc_trim();
}


/*
 * Types
 */

#define TYPE_NIL VLTY_NIL

#define TYPE_CONS VLTY_CONS
struct cons {
	uintptr_t a, d;
};

void cons_ctr(void *ptr, void *arg)
{
	struct cons *d = (struct cons *)ptr;
	struct cons *s = (struct cons *)arg;

	if (arg == NULL) {
		d->a = NIL;
		d->d = NIL;
		return;
	}
	*d = *s;
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
	struct env *orig = (struct env *)arg;
	struct env *new = (struct env *)ptr;

	printf("ARG: %p\n", arg);
	if (arg)
		*new = *orig;
	new->next = orig;
}

static void env_dtr(void *ptr)
{
	struct env *env = (struct env *)ptr;
	struct env_entry *e, *n, *l;
	int i;

	printf("Freeing env %p", env);
	for (i = 0; i < ENV_SIZE; i++) {
		l = env->next ? env->next->table[i] : NULL;
		e = env->table[i];
		while (e != l) {
			printf("%p ", e);
			n = e->next;
			free(e);
			e = n;
		}
		printf(" - ");
	}
	printf("\n");
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
	if (env->next)
		vlvm_gcwk(proc, ((uintptr_t)env->next) | TYPE_ENV);
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

	printf("searching key %lx\n", key);
	n = env_hashf(key);	
	e = env->table[n];
	while (e != NULL) {
		printf("Found %lx (== %lx) at %p\n", e->key, key, e);
		if (e->key == key)
			break;
		e = e->next;
	}

	if (e == NULL)
		evlvm_error("Undefined", key);
	return e->val;
}

void
env_define(struct env *env, uintptr_t key, uintptr_t value)
{
	struct env_entry *e;
	unsigned n = env_hashf(key);

	printf("adding %lx with value %lx\n", key, value);
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
	struct cons cons;
	uintptr_t aptr, dptr;
	vlty aty, dty;
	vlvm_get(&vm, ar, &aty, (void **)&aptr);
	vlvm_get(&vm, dr, &dty, (void **)&dptr);
	cons.a = aptr | aty;
	cons.d = dptr | dty;
	vlvm_new(&vm, dst, TYPE_CONS, &cons);
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
	for (i = 0; i < 1000; i++) {
		cons(2, 1, 2);
	}
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();

	vlvm_psh(&vm, 2);
	vlvm_new(&vm, 2, 0, NULL);

	printf("Push, cleared");
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}
	

	tycache_dumpstats();
	gcrun();
	tycache_dumpstats();
	
	printf("pop: %d", vlvm_pop(&vm, 0));
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
	}
	
	vlvm_new(&vm, 0, 0, NULL);
	printf("Cleared");
	for (i = 0; i < VLVM_NREGS; i++) {
		vlvm_get(&vm, i, &ty, &ptr);
		printf("R%01X:     %p %s\n", i, ptr, vlty_name(ty));
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

	vlvm_new(&vm, 3, TYPE_ENV, NULL);
	tycache_dumpstats();

	vlvm_get(&vm, 3, NULL, (void **)&env1);
	env_define(env1, symbol("X"), symbol("Value of X"));
	env_define(env1, symbol("asd"), symbol("Value of asd"));
	printf("X: %lx\n", env_lookup(env1, symbol("X")));
	printf("Y: %lx\n", env_lookup(env1, symbol("asd")));
	
	vlvm_new(&vm, 4, TYPE_ENV, env1);
	vlvm_get(&vm, 4, NULL, (void **)&env2);
	printf("%p <->  %p\n", env1->table[5], env2->table[5]);
		
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("Y: %lx\n", env_lookup(env2, symbol("asd")));
	env_define(env2, symbol("X"), symbol("New symbol of X"));
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("X: %lx\n", env_lookup(env1, symbol("X")));	

	vlvm_new(&vm, 3, TYPE_NIL, NULL);
	gcrun();
	tycache_dumpstats();
	printf("X: %lx\n", env_lookup(env2, symbol("X")));
	printf("X: %lx\n", env_lookup(env2, symbol("asd")));
	tycache_dumpstats();
	vlvm_new(&vm, 4, TYPE_ENV, env2);
	tycache_dumpstats();
	vlvm_new(&vm, 4, TYPE_NIL, NULL);
	gcrun();
	tycache_dumpstats();
	
	return 0;
}
