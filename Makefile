CC = clang
CFLAGS = -Wall -Wextra -g -std=gnu99 -Wdeclaration-after-statement -Wconversion -Werror -Wshadow -Wpedantic
LDFLAGS = $(shell pkg-config --libs sdl3)
CFLAGS += $(shell pkg-config --cflags sdl3)
TARGET = chips
SRC = src/chips.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

.PHONY: all clean test

all: $(TARGET)

clean:
	rm -f $(TARGET)

test:
