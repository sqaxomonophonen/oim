PKGS=alsa
CFLAGS=-std=c99 -O3 -Wall $(shell pkg-config --cflags $(PKGS))
LINK=$(shell pkg-config --libs $(PKGS)) -lm

all: oscpen pwmpen

oscpen: oscpen.c oim.h
	$(CC) $(CFLAGS) $< -o $@ $(LINK)

pwmpen: pwmpen.c oim.h
	$(CC) $(CFLAGS) $< -o $@ $(LINK)

clean:
	rm -rf *.o oscpen pwmpen
