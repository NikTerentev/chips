CC = clang
CFLAGS = -Wall -Wextra -g -std=c99
LDFLAGS = $(shell pkg-config --libs sdl3)
CFLAGS += $(shell pkg-config --cflags sdl3)
TARGET = chips
SRC = src/chips.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
