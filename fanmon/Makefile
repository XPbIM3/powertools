CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
FRAMEW  = -framework CoreFoundation -framework IOKit
TARGET  = fanmon

all: $(TARGET)

$(TARGET): fanmon.c
	$(CC) $(CFLAGS) -o $(TARGET) fanmon.c $(FRAMEW)

clean:
	rm -f $(TARGET)
