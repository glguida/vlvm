VLVM_SRCDIRS?=../vlvm
VLVM_SRCS=$(VLVM_SRCDIRS)/vlvm.c $(VLVM_SRCDIRS)/slab.c

CFLAGS+= -I. -I $(VLVM_SRCDIRS)
CFLAGS+= -g
CFLAGS+= -O3
CFLAGS+= -pg

vlisp: $(VLVM_SRCS) main.c
	cc $(CFLAGS) -o $@ $(VLVM_SRCS) main.c

clean:
	-rm vlisp
