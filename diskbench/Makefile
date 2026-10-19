CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Подстраховка для сборки на не-младшем тулчейне: Leopard gcc ставит
# подтип ppc7400 (не грузится на G3); force_cpusubtype_ALL даёт подтип
# ppc/ALL — как у канонических Tiger-сборок. На Tiger-тулчейне это коммит-но-оп.
LDFLAGS = -Wl,-force_cpusubtype_ALL
TARGET  = diskbench

all: $(TARGET)

$(TARGET): diskbench.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) diskbench.c

clean:
	rm -f $(TARGET)
