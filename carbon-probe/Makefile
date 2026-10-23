CC      = gcc
# Канон крест-сборки GUI под Panther (собран и проверен 24.10.2026):
# -arch ppc          - кросс-цель
# -mcpu=G3           - слой 1 G3-safe (скилл ppc-build)
# force_cpusubtype_ALL - слой 2, подтип ppc/ALL
# -mmacosx-version-min=10.3 - переносимость на Panther dyld
# -framework Carbon  - UI (программный, без NIB/IB)
CFLAGS  = -std=gnu99 -O1 -Wall
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
               -mmacosx-version-min=10.3
TARGET  = probe
PLIST   = Info.plist
APP     = Probe.app

all: $(TARGET) app

$(TARGET): probe.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) probe.c -framework Carbon

# минимальный бандл: нужен только для запуска через `open` по ssh
# (LaunchServices поднимает процесс в консольной сессии - голый бинарь
# из-под sshd до WindowServer не достучается корректно)
app: $(TARGET)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET)
	rm -rf $(APP)
