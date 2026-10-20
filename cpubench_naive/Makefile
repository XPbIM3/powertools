# naive.c - unoptimised FP/INT throughput, one source for both targets
CC      = gcc
CFLAGS  = -std=gnu99 -O0 -Wall
# Арка задаёт имя бинаря: один исходник, два артефакта (решение 20.10.2026)
ARCH    := $(shell uname -m)
ifeq (i386,$(ARCH))
TARGET  = naive-intel
else
TARGET  = naive-ppc
# G3-safe обязателен, два слоя: -mcpu=G3 + force_cpusubtype_ALL (скилл ppc-build)
TARGET_FLAGS = -mcpu=G3 -Wl,-force_cpusubtype_ALL
endif

all: $(TARGET)

$(TARGET): naive.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) naive.c

run: $(TARGET)
	./$(TARGET) $(N)

clean:
	rm -f naive-ppc naive-intel
