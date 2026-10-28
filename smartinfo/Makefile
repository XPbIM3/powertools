CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Три слоя G3-safe, флаги ОДИНАКОВЫ на обоих хостах (канон пересборки -
# кросс на intel-leopard, на PPC-хосте -arch ppc = родная цель):
#   -arch ppc                    кросс-цель ppc на intel-хосте;
#   -mcpu=G3                     инструкции/шедулер 750, без AltiVec;
#   -Wl,-force_cpusubtype_ALL    подтип Mach-O ppc/ALL (иначе ppc7400);
#   -mmacosx-version-min=10.2    dyld Jaguar: SMART-API есть уже в 10.2,
#                                min>=10.3 втачивает libmx.A.dylib,
#                                которой на Jaguar нет.
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2
FRAMEWORKS   = -framework IOKit -framework CoreFoundation
TARGET  = smartinfo

all: $(TARGET)

# smartcore - сам SMART-зонд (его же зовёт DiskGraph.app), smartinfo.c -
# консольная обёртка: таблица, -a, коды возврата
SRCS    = smartinfo.c smartcore.c

$(TARGET): $(SRCS) smartcore.h
	$(CC) $(CFLAGS) $(TARGET_FLAGS) $(FRAMEWORKS) -o $(TARGET) $(SRCS)

# Intel-сборка отдельная, не universal: своя цель, чтобы ppc-бинарь
# не тянул minos 10.4 и оставался годным для Jaguar/Panther.
intel: smartinfo-i386

smartinfo-i386: $(SRCS) smartcore.h
	$(CC) $(CFLAGS) -arch i386 -mmacosx-version-min=10.4 $(FRAMEWORKS) -o smartinfo-i386 $(SRCS)

clean:
	rm -f $(TARGET) smartinfo-i386
