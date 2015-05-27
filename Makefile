CFLAGS += -Wall -g -O0 -fPIC

all: libvivwrap.so

CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

OBJS = wrap.o

libvivwrap.so: $(OBJS)
	$(CC) -g -O0 -Wall -shared -o $@ $^ -ldl -fPIC

clean:
	rm -f *.P
	rm -f *.so
	rm -f *.o
