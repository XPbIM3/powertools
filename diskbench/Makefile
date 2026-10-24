CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Три слоя G3-safe, флаги ОДИНАКОВЫ на обоих хостах (канон пересборки -
# кросс на intel-leopard, на PPC-хосте -arch ppc = родная цель):
# 1) -arch ppc - кросс-цель ppc на intel-хосте (Leopard gcc без неё не
#    принимает -mcpu=G3);
# 2) -mcpu=G3 - набор инструкций и шедулер 750 (дефолты тулчейнов -
#    G4/G5-класс, с ними бинарь падает с Illegal instruction на G3 и тащит
#    AltiVec-риск);
# 3) -Wl,-force_cpusubtype_ALL - подтип Mach-O ppc/ALL (Leopard-дефолт
#    ppc7400 на G3 не грузится);
# 4) -mmacosx-version-min=10.2 - минимальная ОС Jaguar: кросс с Leopard
#    без minos тянет символы libSystem, которых нет на старых системах
#    (ловили на pimem: ___stderrp). НЕ ПОДНИМАТЬ до 10.3+: leopard-драйвер
#    при min>=10.3 втачивает в бинарь /usr/lib/libmx.A.dylib, которой на
#    10.2 НЕТ - dyld отказывается грузить бинарь (ловили 25.10.2026,
#    diskbench-бинарь Leopard-сборки: "can't open library: libmx.A.dylib").
#    min=10.2 линкует ТОЛЬКО libSystem - Jaguar глотает чисто.
# Результат: один бинарь одинаков на Jaguar 10.2 ... G5 ... Leopard.
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2
TARGET  = diskbench

all: $(TARGET)

$(TARGET): diskbench.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) diskbench.c

clean:
	rm -f $(TARGET)
