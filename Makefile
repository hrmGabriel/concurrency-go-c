CC = gcc
CFLAGS = -O2 -fopenmp -Wall -Wextra -Werror

.PHONY: all clean
all: build/c build/go

build/c: c/main.c c/hybrid.c
	@mkdir -p build
	$(CC) $(CFLAGS) c/hybrid.c -lm -o $@

build/go: go/go.mod $(wildcard go/*.go)
	@mkdir -p build
	cd go && CGO_ENABLED=0 go build -trimpath -o ../build/go .

clean:
	rm -f build/c build/go
