# pimem.c - Ooura pi (SuperPI-class) tiers + MEM phase, one source, two targets
# Уровень оптимизации ЗАФИКСИРОВАН: -O1 (свип всех уровней 22.10.2026:
# O0 замедляет pi на Core 2 в 2.7x, на G3 в 1.8x; O1-O3-Os в пределах +-4%;
# выбор -O1 = совпадение с фольклором SuperPI + честные числа на G3).
CC      = gcc
CFLAGS  = -std=gnu99 -O1 -Wall -fno-tree-vectorize \
          -funroll-loops -fomit-frame-pointer
# G3-safe PPC: три слоя (скилл ppc-build) + -arch ppc кросс-цель.
# Фикс 25.10.2026: без -arch ppc на intel-хосте -mcpu=G3 не проходил
# (cc1: invalid option 'cpu=G3') - make падал, хотя readme канонил кросс
# именно оттуда. На PPC-хосте -arch ppc = родная цель, флаги безвредны.
PPC_FLAGS = -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.3
# x86-only: без него PPC-ветка не собирается
INT_FLAGS = -malign-double

ARCH := $(shell uname -m)
ifeq (i386,$(ARCH))
# кросс-хост (intel-leopard): обе цели, две компилированные версии
all: pimem-intel pimem-ppc
else
# PPC-хост: только родная цель (канонная пересборка на Tiger при пробуждении)
all: pimem-ppc
endif

pimem-intel: pimem.c
	$(CC) $(CFLAGS) $(INT_FLAGS) -o $@ pimem.c -lm

pimem-ppc: pimem.c
	$(CC) $(CFLAGS) $(PPC_FLAGS) -o $@ pimem.c -lm

clean:
	rm -f pimem-ppc pimem-intel
