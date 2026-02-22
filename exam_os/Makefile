CC      = gcc
CFLAGS  = -Wall -Wextra -Iinclude -pthread
LDFLAGS = -lncurses -pthread -lrt

SRC = src/main.c \
      src/config.c \
      src/logger.c \
      src/scheduler.c \
      src/memory.c \
      src/io_buffer.c \
      src/interrupt.c \
      src/dashboard.c

OUT = exam_os

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC) $(LDFLAGS)

clean:
	rm -f $(OUT) output/*.txt output/*.json

run: all
	./$(OUT)

demo: all
	./$(OUT) --demo

.PHONY: all clean run demo