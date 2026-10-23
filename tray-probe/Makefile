# tray.m - menu-bar status item probe for Mac OS X 10.3.9 (panther)
# Кросс-сборка на intel-leopard (канон + objc-легаси ABI):
#   gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3 \
#       -std=gnu99 -O1 -Wall -fobjc-abi-version=1 -x objective-c \
#       tray.m -o tray -framework Foundation -framework AppKit
CC      = gcc
CFLAGS  = -std=gnu99 -O1 -Wall -fobjc-abi-version=1
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
               -mmacosx-version-min=10.3 -x objective-c
TARGET  = tray
PLIST   = Info.plist
APP     = TrayProbe.app

all: $(TARGET) app

$(TARGET): tray.m
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) tray.m \
	      -framework Foundation -framework AppKit

# минимальный бандл: бес него через ssh не запустишь (open = LaunchServices
# = консольная сессия)
app: $(TARGET)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET)
	rm -rf $(APP)
