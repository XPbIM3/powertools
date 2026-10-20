CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall -faltivec
# cpuburn - PPC-only инструмент (режим vec бессмыслен на Intel), но бинарь
# обязан грузиться на ВСЕХ PPC парка (G3..G5). G3-safe, два слоя (скилл
# ppc-build): -mcpu=G3 держит скалярные части и закрыает VA-кодогенерацию
# вне builtin-вызовов; force_cpusubtype_ALL = подтип ppc/ALL. AltiVec
# инструкции остаются только в vec-путях за runtime-guard'ом
# hw.optional.altivec (G3 их не исполняет никогда).
TARGET_FLAGS = -mcpu=G3 -Wl,-force_cpusubtype_ALL
TARGET  = cpuburn

all: $(TARGET)

$(TARGET): cpuburn.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) cpuburn.c

clean:
	rm -f $(TARGET)
