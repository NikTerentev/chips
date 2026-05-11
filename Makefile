CC = clang
CFLAGS = -Wall -Wextra -g -std=c99 -Wdeclaration-after-statement
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
