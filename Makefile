OBJECTS=main.o m68k.o m68k_trace.o mem.o debugger.o console.o ramdisk.o
CFLAGS=-Wall -Wextra -DCPU_BREAKPOINT -DCPU_TRACE

all: cpm68emu

cpm68emu: ${OBJECTS}
	gcc -o cpm68emu $^ ${LDFLAGS}

main.o: main.c
	gcc -c $^ ${CFLAGS}

m68k.o: m68k.c
	gcc -c $^ ${CFLAGS}

m68k_trace.o: m68k_trace.c
	gcc -c $^ ${CFLAGS}

debugger.o: debugger.c
	gcc -c $^ ${CFLAGS}

mem.o: mem.c
	gcc -c $^ ${CFLAGS}

console.o: console.c
	gcc -c $^ ${CFLAGS}

ramdisk.o: ramdisk.c
	gcc -c $^ ${CFLAGS}

.PHONY: clean
clean:
	rm -f *.o cpm68emu

