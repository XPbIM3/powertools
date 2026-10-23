# diskgauge.m - live disk I/O gauge in the menu bar (tray) for 10.3.9
# Cross-built on intel-leopard (10.5 intel host -> 10.3 ppc target),
# canon park flags + legacy objc1 ABI. IOKit is the added framework.
#   gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3 \
#       -std=gnu99 -O1 -Wall -fobjc-abi-version=1 -x objective-c \
#       diskgauge.m -o diskgauge \
#       -framework Foundation -framework AppKit -framework IOKit
CC      = gcc
CFLAGS  = -std=gnu99 -O1 -Wall -fobjc-abi-version=1
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
               -mmacosx-version-min=10.3 -x objective-c
TARGET  = diskgauge
PROBE   = dgprobe
PLIST   = Info.plist
APP     = DiskGauge.app

all: $(TARGET) $(PROBE) app

$(TARGET): diskgauge.m
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) diskgauge.m \
	      -framework Foundation -framework AppKit -framework IOKit

# CLI-пробник тех же счётчиков - объективная кросс-сверка трей-цифр
# по ssh (без AppKit).
$(PROBE): dgprobe.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(PROBE) dgprobe.c \
	      -framework CoreFoundation -framework IOKit

# минимальный бандл: бес него через ssh не запустишь (open = LaunchServices
# = консольная сессия). LSUIElement=1 в plist -> агент без док-иконки.
app: $(TARGET)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET) $(PROBE)
	rm -rf $(APP)
