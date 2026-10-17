CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
TARGET  = diskbench

all: $(TARGET)

$(TARGET): diskbench.c
	$(CC) $(CFLAGS) -o $(TARGET) diskbench.c

clean:
	rm -f $(TARGET)
