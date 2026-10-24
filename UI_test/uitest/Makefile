# uitest - universal tray skeleton SMOKE (no hardware layer)
# One source -> two slices -> fat binary, from the 10.5 intel host.
#   PPC slice  (10.2-10.4 G3/G4): canon park cross flags + objc1
#   i386 slice (10.5 intel):      native leopard
#   lipo -create -> uitest (fat, runs on BOTH park machines)
CC     = gcc
CFLAGS = -std=gnu99 -O1 -Wall
PPC_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
            -mmacosx-version-min=10.3 -fobjc-abi-version=1 \
            -x objective-c
I386_FLAGS = -arch i386 -mmacosx-version-min=10.5 -x objective-c
TARGET = uitest
PLIST  = Info.plist
APP    = UITest.app

all: fat app

$(TARGET)-ppc: uitest.m
	$(CC) $(CFLAGS) $(PPC_FLAGS) -o $(TARGET)-ppc uitest.m \
	      -framework Foundation -framework AppKit

$(TARGET)-i386: uitest.m
	$(CC) $(CFLAGS) $(I386_FLAGS) -o $(TARGET)-i386 uitest.m \
	      -framework Foundation -framework AppKit

fat: $(TARGET)-ppc $(TARGET)-i386
	lipo -create -output $(TARGET) $(TARGET)-ppc $(TARGET)-i386

app: fat
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET) $(TARGET)-ppc $(TARGET)-i386
	rm -rf $(APP)
