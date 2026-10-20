CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Арка задаёт имя бинаря: один исходник, два артефакта (решение 20.10.2026)
ARCH    := $(shell uname -m)
ifeq (i386,$(ARCH))
TARGET  = cpubench-intel
else
TARGET  = cpubench-ppc
# G3-safe обязателен: два слоя. 1) -mcpu=G3 — набор инструкций и шедулер
# (без него тулчейн дефолтится на G4/G5-класс); 2) force_cpusubtype_ALL —
# подтип Mach-O ppc/ALL (Leopard-дефолт ppc7400 на G3 не грузится).
# Результат: один бинарь честно одинаковый на G3/G4/G5 (скилл ppc-build).
TARGET_FLAGS = -mcpu=G3 -Wl,-force_cpusubtype_ALL
endif

all: $(TARGET)

$(TARGET): cpubench.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) cpubench.c

clean:
	rm -f cpubench-ppc cpubench-intel cpubench
