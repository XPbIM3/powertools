CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Три слоя G3-safe, флаги ОДИНАКОВЫ на обоих хостах (канон пересборки -
# кросс на intel-leopard; на PPC-хосте -arch ppc = родная цель):
#   -arch ppc                    кросс-цель ppc на intel-хосте (Leopard gcc
#                                без неё не принимает -mcpu=G3);
#   -mcpu=G3                     инструкции/шедулер 750;
#   -Wl,-force_cpusubtype_ALL    подтип Mach-O ppc/ALL;
#   -mmacosx-version-min=10.3    dyld Panther (___stderrp без minos).
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3
TARGET  = batinfo

all: $(TARGET)

$(TARGET): batinfo.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) batinfo.c -framework CoreFoundation -framework IOKit

clean:
	rm -f $(TARGET)
