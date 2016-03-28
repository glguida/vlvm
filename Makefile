CFLAGS= -Ivlvm -Ivlisp
VPATH= vlvm vlisp


main: vlvm.c slab.c main.c
	$(CC) $(CFLAGS) $^ -o $@
