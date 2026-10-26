CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# cpuburn - PPC-only инструмент (решение 22.10.2026: AltiVec удалён
# полностью - код-пути vec, runtime-guard и -faltivec выкинуты; сборка
# БЕЗ AltiVec по умолчанию).
# Три слоя G3-safe, флаги ОДИНАКОВЫ на обоих хостах (канон пересборки -
# кросс на intel-leopard; на PPC-хосте -arch ppc = родная цель):
#   -arch ppc                    кросс-цель ppc на intel-хосте ( Leopard gcc
#                                без неё не принимает -mcpu=G3 вообще);
#   -mcpu=G3                     инструкции/шедулер 750 (слой 1) - без него
#                                дефолты тулчейнов дают G4+-код: Illegal
#                                instruction на G3;
#   -Wl,-force_cpusubtype_ALL    подтип Mach-O ppc/ALL (слой 2) - Leopard-,
#                                дефолт ppc7400 на G3 не грузится;
#   -mmacosx-version-min=10.3    dyld Panther (слой 3): кросс с Leopard без
#                                minos тянет символы libSystem, которых нет
#                                на 10.3 - ловили на pimem (___stderrp).
# VA-мнемоник в бинаре НЕ ДОЛЖНО БЫТЬ (проверять otool -tv: 0).
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3
TARGET  = cpuburn

all: $(TARGET)

$(TARGET): cpuburn.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) cpuburn.c

clean:
	rm -f $(TARGET)
