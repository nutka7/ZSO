TARGET = loader emu
CC = gcc
CFLAGS = -g -Wall

.PHONY: default all clean

all: $(TARGET)

loader: loader.c emuerr.h
	$(CC) $(CFLAGS) $< -o $@

emu: emu.c emuerr.h
	$(CC) $(CFLAGS) $< -o $@ -lncurses

clean:
	-rm -f *.o
	-rm -f $(TARGET)
