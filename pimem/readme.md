# pimem

Один бинарь — два мира: **π-ступени (SuperPI-класс)** и **MEM-фаза**.
Один исходник `pimem.c`, две скомпилированные версии в гите — ppc и intel.
Универсальность: G3 750 (Panther 10.3) → G4/G5 (Tiger/Leopard) → Core 2
(Leopard i386), одинаковые правила расчёта на всём парке.

## Что внутри

- **π-фаза** — код T.Ooura (pi_fftcs + fftsg, форм. AGM/FFT, ядра не тронуто,
  лицензионная шапка сохранена), собранный под `-O1` — цифры совпадают с
  фольклором SuperPI (3.50 s / 1048576 на Core2-2.4). Ступени 128k/256k/
  512k/1M знаков, на каждую печатается время + чексумма цифр:
  побитовая сверка G3↔Core2 — чек-суммы и `-o`-файлы идентичны.
- **MEM-фаза** — наивный C-мир памяти: LCG-заполн, прогрев, честный copy,
  Sattolo-шаффл (того же LCG), полный dependent-load walk, чек-суммы.
  Буферы `volatile` — под `-O2/-O1` компилятор не сворачивает copy
  в `memcpy`; pprint: 1517 MB/s на Core2 @2.4.

## Запуск

```
pimem                   # полный: 4 ступени π + MEM copy 10с + walk
pimem -o FILE           # + знаки π всех ступеней один файл (сверка арок)
pimem -w MB             # окно MEM-буферов (сумма ОБОИХ, дефолт 128)
pimem -n                # proof: copy прибит к 32 пассам
pimem -t S              # длина copy-фазы
```

Машины с ≤192 МБ RAM — `-w 64`.

## Как собрано (записка 22.10.2026)

Кросс-сборка на **intel-leopard**, `i686-apple-darwin9-gcc-4.0.1 (build 5493)`,
обе цели из одного исходника:

```
gcc  -std=gnu99 -O1 -Wall -fno-tree-vectorize -funroll-loops \
     -fomit-frame-pointer -malign-double pimem.c -lm -o pimem-intel
gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
    -mmacosx-version-min=10.3 -std=gnu99 -O1 -Wall -fno-tree-vectorize \
    -funroll-loops -fomit-frame-pointer pimem.c -lm -o pimem-ppc
```

Флаговые заметки:
- `-O1` (а не `-O2`): на π-ядре Ooura `-O1` даёт 3.50 s против 3.65 при
  `-O2` (два подряда идентичных прогона) — так держим фольклорную точку
  SuperPI. `-funroll-loops -fomit-frame-pointer -malign-double` — родные
  флаги из Makefile SuperPI; `-malign-double` x86-only, в PPC-ветке
  отсутствует (`-malign-double` на ppc-gcc = error).
- Два слоя G3-safe на месте: `otool -hv` = **PPC ALL**, `otool -tv` —
  VA-мнемоник **0**; без `-mmacosx-version-min=10.3` бинарь не грузится
  на Panther (dyld: undefined `___stderrp`).
- Канон сборки — **intel-leopard** (i386-gcc-4.0.1, build 5493) — кроссит
  ОБЕ цели штатно, проверено прогонами на обеих арках (решение пользователи
  22.10.2026: "собирать можно на интеле — он норм это делает").

## Верификация (22.10.2026)

| проверка | результат |
|---|---|
| otool подтип ppc | **PPC ALL** ✓ |
| AltiVec-мнемоники | **0** ✓ |
| знаки π, 4 ступени, G3↔Core2 | байт-в-байт (chk `f71873c3`, `cc217b24`, `ab84e40c`, `0fdf413c`; `-o`-файлы cmp = identical) ✓ |
| MEM-чексумма `-w` по умолч. | `b27d388c` ✓ |
| MEM-чексумма `-w 64` | `b2e16b15` ✓ |
| `/tmp` walk | 95.6 ns/hop (Core2), 512.6 (G3-500@`-w 64`) — платные латентности ✓ |

## Артефакты

- `pimem-intel` md5 `9e75e78a84a33a0f1a206af48f6047ce`
- `pimem-ppc`   md5 `477170e18ac62fcc1324f84e7ab3ee4e`

Базовые цифры: `~/PowerTools/pimem_baseline.md` (лаконичная таблица,
наполняется по мере обмеров машин).
