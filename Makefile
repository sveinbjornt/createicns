CFLAGS = -arch arm64 -arch x86_64

all: createicns readicns

createicns: createicns.c

readicns: readicns.c

.PHONY: all clean
clean:
	-rm -f createicns readicns $(objects)
