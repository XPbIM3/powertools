CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Арка задаёт имя бинаря: один исходник, два артефакта (решение 20.10.2026)
ARCH    := $(shell uname -m)
ifeq (i386,$(ARCH))
TARGET  = cpubench-intel
else
TARGET  = cpubench-ppc
# Подстраховка для сборки на не-младшем тулчейне (Leopard-ppc и т.п.):
# force_cpusubtype_ALL даёт подтип ppc/ALL — как у канонических Tiger-сборок.
LDFLAGS = -Wl,-force_cpusubtype_ALL
endif

all: $(TARGET)

$(TARGET): cpubench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) cpubench.c

clean:
	rm -f cpubench-ppc cpubench-intel cpubench
