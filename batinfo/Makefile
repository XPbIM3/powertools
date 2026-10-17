CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
TARGET  = batinfo

all: $(TARGET)

$(TARGET): batinfo.c
	$(CC) $(CFLAGS) -o $(TARGET) batinfo.c -framework CoreFoundation -framework IOKit

clean:
	rm -f $(TARGET)
