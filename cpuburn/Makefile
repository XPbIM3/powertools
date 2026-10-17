CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall -faltivec
TARGET  = cpuburn

all: $(TARGET)

$(TARGET): cpuburn.c
	$(CC) $(CFLAGS) -o $(TARGET) cpuburn.c

clean:
	rm -f $(TARGET)
