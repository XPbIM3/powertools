CC      = gcc
CFLAGS  = -std=gnu99 -O2 -Wall
# Три слоя G3-safe + кросс-цель + minos - ОДИНАКОВЫ на обоих хостах
# (канон репо; см. diskbench/Makefile и скилл ppc-build):
# 1) -arch ppc     - кросс-цель ppc на intel-хосте;
# 2) -mcpu=G3      - инструкции и шедулер 750, без AltiVec-кода;
# 3) -force_cpusubtype_ALL - подтип ppc/ALL, грузится на G3;
# 4) -mmacosx-version-min=10.2 - одна libSystem, Jaguar..Leopard.
TARGET_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2
TARGET      = dbsweep
LIN         = dblin
RND         = dbrnd
PROBE       = graphprobe
APP         = GraphProbe.app
APP2        = DiskGraph.app
APPFLAGS    = $(CFLAGS) $(TARGET_FLAGS) -fobjc-abi-version=1 \
              -x objective-c \
              -framework Foundation -framework AppKit -framework Security \
              -framework IOKit

all: $(TARGET) $(LIN) $(RND) $(APP) $(APP2)

$(TARGET): dbsweep.c
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(TARGET) dbsweep.c

# консольные тулзы: тонкие обёртки над общими ядрами dglincore/dgrndcore
# (те же ядра зовёт UI). Человекочитаемый stdout, ноль CSV.
# dgdev - общее: часы, открытие raw-ноды, ёмкость, перцентили
$(LIN): dblin.c dglincore.c dglincore.h dgdev.c dgdev.h
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(LIN) dblin.c dglincore.c dgdev.c

$(RND): dbrnd.c dgrndcore.c dgrndcore.h dgdev.c dgdev.h
	$(CC) $(CFLAGS) $(TARGET_FLAGS) -o $(RND) dbrnd.c dgrndcore.c dgdev.c

# ступень 2: пробник рисования (окно NSView + drawRect) - objc1
# конструкции, кросс с Leopard на все клетки 10.2..10.5
$(APP): $(PROBE).m Info.plist
	$(CC) $(APPFLAGS) -o $(PROBE) $(PROBE).m
	@mkdir -p $(APP)/Contents/MacOS
	@cp Info.plist $(APP)/Contents/
	@cp $(PROBE) $(APP)/Contents/MacOS/

# ступень 4b: UI зовёт ядра dglincore/dgrndcore напрямую (pthread),
# без внешних процессов и stdout-обмена
$(APP2): diskgraph.m dglincore.c dgrndcore.c dglincore.h dgrndcore.h \
         dgdev.c dgdev.h Info-diskgraph.plist
	$(CC) $(APPFLAGS) diskgraph.m -x none dglincore.c dgrndcore.c dgdev.c \
	      -lpthread -o diskgraph
	@mkdir -p $(APP2)/Contents/MacOS $(APP2)/Contents/Resources
	@cp Info-diskgraph.plist $(APP2)/Contents/Info.plist
	@cp diskgraph $(APP2)/Contents/MacOS/

clean:
	rm -f $(TARGET) $(LIN) $(RND) $(PROBE) diskgraph
	rm -rf $(APP) $(APP2)
