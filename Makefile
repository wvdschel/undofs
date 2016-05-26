LIBS=$(shell pkg-config --libs fuse)
CFLAGS=-g -O0 $(shell pkg-config --cflags fuse) -std=c99 -MD
CC=clang

OBJECTS=undofs.o undofs_util.o undofs_fops.o

AUTODEPS=$(patsubst %.o,%.d,$(OBJECTS))

all: undofs

release: CFLAGS+=-DNOLOG
release: all

-include $(AUTODEPS)

clean:
	@rm -rf undofs $(OBJECTS) $(AUTODEPS)

undofs: $(OBJECTS)
	$(CC) $(CFLAGS) $(LIBS) $^ -o $@
