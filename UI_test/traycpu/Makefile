# traycpu.m - CPU load gauge in the menu bar (tray) for Mac OS X 10.3.9
# Cross-built on intel-leopard (10.5 intel host -> 10.3 ppc target),
# canon park flags + legacy objc1 ABI.
#   gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3 \
#       -std=gnu99 -O1 -Wall -fobjc-abi-version=1 -x objective-c \
#       traycpu.m -o traycpu -framework Foundation -framework AppKit
CC      = gcc
CFLAGS  = -std=gnu99 -O1 -Wall -fobjc-abi-version=1
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
               -mmacosx-version-min=10.3 -x objective-c
TARGET  = traycpu
PLIST   = Info.plist
APP     = TrayCPU.app

all: $(TARGET) app

$(TARGET): traycpu.m
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) traycpu.m \
	      -framework Foundation -framework AppKit

# минимальный бандл: бес него через ssh не запустишь (open = LaunchServices
# = консольная сессия). LSUIElement=1 в plist -> агент без док-иконки.
app: $(TARGET)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET)
	rm -rf $(APP)
