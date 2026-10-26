CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
FRAMEW  = -framework CoreFoundation -framework IOKit
# Три слоя G3-safe, флаги ОДИНАКОВЫ на обоих хостах (канон пересборки -
# кросс на intel-leopard; на PPC-хосте -arch ppc = родная цель):
#   -arch ppc                    кросс-цель ppc на intel-хосте (Leopard gcc
#                                без неё не принимает -mcpu=G3);
#   -mcpu=G3                     инструкции/шедулер 750 (дефолт тулчейна -
#                                G4+/G5-класс: Illegal instruction на G3);
#   -Wl,-force_cpusubtype_ALL    подтип Mach-O ppc/ALL (Leopard-дефолт
#                                ppc7400 на G3 не грузится);
#   -mmacosx-version-min=10.3    dyld Panther: кросс с Leopard без minos
#                                тянет символы, которых нет на 10.3
#                                (___stderrp). Framework-линковка слоёв не
#                                ломает: IOKit/CF - публичные, древние.
# Работает на Tiger/Leopard PPC; перечисленные IOKit-вызовы - от G3+.
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3
TARGET  = fanmon

all: $(TARGET)

$(TARGET): fanmon.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) fanmon.c $(FRAMEW)

clean:
	rm -f $(TARGET)
