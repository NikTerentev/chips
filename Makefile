CC = clang
BASE_FLAGS = -Wall -Wextra -std=gnu99 -Wdeclaration-after-statement -Wconversion -Werror -Wshadow -Wpedantic -Wredundant-decls -Wwrite-strings -Wcast-qual -Wformat -Wformat-security -Wdouble-promotion -Wnull-dereference -Wmissing-prototypes
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

LDFLAGS = $(SDL_LIBS)
TARGET = chips
SRC = src/chips.c

.PHONY: all debug release clean test

all: debug

debug: CFLAGS = $(BASE_FLAGS) -g -O0 $(SDL_CFLAGS)
debug: $(TARGET)

release: CFLAGS = $(BASE_FLAGS) -O2 -DNDEBUG $(SDL_CFLAGS)
release: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

test:
