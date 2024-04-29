CFLAGS+= -Wall -Werror -std=gnu99 -g
LDFLAGS=-pthread

HW=prgsem
BINARIES=prgsem-main

CFLAGS+=$(shell sdl2-config --cflags)
LDFLAGS+=$(shell sdl2-config --libs) -lSDL2_image


all: $(BINARIES)

OBJS=$(patsubst %.c,%.o,$(wildcard *.c))

prgsem-main: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(OBJS): %.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) $(OBJS)
