all: smd client

DEBUG=-g -fstack-protector-all -Wstack-protector -fno-omit-frame-pointer -fPIC -DDEBUG
CFLAGS=$(shell pkg-config --cflags libspotify alsa) -Wall $(DEBUG) -pthread
LDFLAGS=$(shell pkg-config --libs-only-L libspotify alsa) -g -pthread
LDLIBS=$(shell pkg-config --libs-only-l libspotify alsa) -pthread -lm

.PHONY: all clean

smd: smd.o audio.o

client: client.o

clean:
	rm -f smd client
	rm -f *.o
