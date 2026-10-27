CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Три слоя G3-safe + кросс-цель + minos - ОДИНАКОВЫ на обоих хостах
# (канон репо; см. diskbench/Makefile и скилл ppc-build):
# 1) -arch ppc     - кросс-цель ppc на intel-хосте;
# 2) -mcpu=G3      - инструкции и шедулер 750, без AltiVec-кода;
# 3) -force_cpusubtype_ALL - подтип ppc/ALL, грузится на G3;
# 4) -mmacosx-version-min=10.2 - одна libSystem, Jaguar..Leopard.
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2
TARGET  = dbsweep

all: $(TARGET)

$(TARGET): dbsweep.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) dbsweep.c

clean:
	rm -f $(TARGET)
