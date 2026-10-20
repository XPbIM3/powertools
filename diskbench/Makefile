CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# G3-safe обязателен, два слоя (скилл ppc-build): 1) -mcpu=G3 — набор
# инструкций и шедулер 750 (дефолты тулчейнов — G4/G5-класс, с ними бинарь
# падает с Illegal instruction на G3 и тащит AltiVec-риск); 2)
# force_cpusubtype_ALL — подтип Mach-O ppc/ALL (Leopard-дефолт ppc7400
# на G3 не грузится). Результат: один бинарь одинаков на G3/G4/G5.
TARGET_FLAGS = -mcpu=G3 -Wl,-force_cpusubtype_ALL
TARGET  = diskbench

all: $(TARGET)

$(TARGET): diskbench.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) diskbench.c

clean:
	rm -f $(TARGET)
