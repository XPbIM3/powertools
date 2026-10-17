CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
TARGET  = cpubench

all: $(TARGET)

$(TARGET): cpubench.c
	$(CC) $(CFLAGS) -o $(TARGET) cpubench.c

clean:
	rm -f $(TARGET)
