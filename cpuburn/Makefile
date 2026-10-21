CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# cpuburn - PPC-only инструмент (решение 22.10.2026: AltiVec удалён
# полностью - код-пути vec, runtime-guard и -faltivec выкинуты; сборка
# БЕЗ AltiVec по умолчанию). Два слоя G3-safe (скилл ppc-build):
# -mcpu=G3 и force_cpusubtype_ALL = подтип ppc/ALL; VA-мнемоник в
# бинаре НЕ ДОЛЖНО БЫТЬ (проверять otool -tv: 0).
TARGET_FLAGS = -mcpu=G3 -Wl,-force_cpusubtype_ALL
TARGET  = cpuburn

all: $(TARGET)

$(TARGET): cpuburn.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) cpuburn.c

clean:
	rm -f $(TARGET)
