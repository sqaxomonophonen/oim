BASE_CFLAGS=-std=gnu99 -O3 -Wall
BASE_LINK=-lm
PKGS=alsa
CFLAGS=${BASE_CFLAGS} $(shell pkg-config --cflags $(PKGS))
LINK=$(shell pkg-config --libs $(PKGS)) ${BASE_LINK}

all: oscpen pwmpen pwmarp tablet-osc

oscpen: oscpen.c oim.h
	$(CC) $(CFLAGS) $< -o $@ $(LINK)

pwmpen: pwmpen.c oim.h
	$(CC) $(CFLAGS) $< -o $@ $(LINK)

pwmarp: pwmarp.c oim.h
	$(CC) $(CFLAGS) $< -o $@ $(LINK)

tablet-osc: tablet-osc.c
	$(CC) $(BASE_CFLAGS) $< -o $@ $(BASE_LINK)

clean:
	rm -rf *.o oscpen pwmpen pwmarp
