CFLAGS+= -Wall -Werror -std=gnu99 -g
LDFLAGS=-pthread -lm

HW=prgsem
BINARIES=prgsem-main module

CFLAGS+=$(shell sdl2-config --cflags)
LDFLAGS+=$(shell sdl2-config --libs) -lSDL2_image 


all: $(BINARIES)

OBJS=$(patsubst %.c,%.o,$(wildcard *.c))

prgsem-main: $(OBJS)
	$(CC) prg_io_nonblock.o messages.o threads.o xwin_sdl.o $(LDFLAGS) -o $@ 

module: $(OBJS)
	$(CC) prg_io_nonblock.o messages.o module.o $(LDFLAGS) -o $@
$(OBJS): %.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) $(OBJS)


ZIPFILE = archive.zip
zip: clean all
	zip $(ZIPFILE) *