# tempgauge - CPU temp gauge in the tray for 10.5 (leopard)
# NATIVE build on the very host it monitors (intel-leopard,
# MacBookPro3,1): no cross flags, no -arch p pc.  Same skeleton and
# plist recipe as diskgauge; SMC user-client counter source.
#   gcc -std=gnu99 -O1 -Wall tempgauge.m -o tempgauge \
#       -framework Foundation -framework AppKit -framework IOKit
CC     = gcc
CFLAGS = -std=gnu99 -O1 -Wall
TARGET = tempgauge
PROBE  = tmpprobe
PLIST  = Info.plist
APP    = TempGauge.app

all: $(TARGET) $(PROBE) app

$(TARGET): tempgauge.m
	$(CC) $(CFLAGS) -o $(TARGET) tempgauge.m \
	      -framework Foundation -framework AppKit -framework IOKit

# CLI-пробник - ходит по всему SMC key space по индексу; разведка
# (какие ключи температурные на данной машине) сделана ним же.
$(PROBE): tmpprobe.c
	$(CC) $(CFLAGS) -o $(PROBE) tmpprobe.c \
	      -framework CoreFoundation -framework IOKit

app: $(TARGET)
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS
	cp $(PLIST) $(APP)/Contents/Info.plist
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	touch $(APP)

clean:
	rm -f $(TARGET) $(PROBE)
	rm -rf $(APP)
