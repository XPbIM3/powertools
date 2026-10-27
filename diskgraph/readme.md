# diskgraph

Дисковый бенчмарк с графиком по поверхности: raw-девайс, только чтение.

- движок `dbsweep` (CLI, без AppKit)
- UI `DiskGraph.app` (окно + 2 панели графиков, движок под root)
- пробник клетки UI-матрицы `graphprobe` (окно+NSView+drawRect)

Задача: линейный пунктирный свип вдоль всей ёмкости (скорость MB/s vs
место на диске) + random read (лаг мкс vs место). Job HD Tune-стилe.

## Почему raw-девайс и только чтение

- raw `/dev/rdiskN` даёт точное LBA-позиционирование - ось X обоих
  графиков = настоящее место на диске, зонную запись (внешние зоны
  быстрее внутренних) видно честно, файл на файловой системе дал бы
  фрагментированное «приближение»
- только чтение = ноль риска попортить данные, никакой предзаписи
  чанка перед random-фазой (обычная цена честного random по всему
  диску - писать весь объём; на raw можно не писать вообще)
- raw-ноды root:operator 640 - движок запускается под root через
  AuthorizationExecuteWithPrivileges (пароль-диалог с 10.2)

## dbsweep - движок

```
usage: sudo dbsweep -d /dev/rdiskN [options]
  -c SIZE   linear segment size    (default 32M, mult of -b)
  -D N      duty percent 10..100   (default 33; 100 = solid)
  -b SIZE   linear read block      (default 1M, pow2)
  -B SIZE   random read block      (default 4K)
  -T N      random run seconds     (default 30, 0 = skip)
  -m SIZE   limit sweep extent     (0 = whole capacity)
  -w N      random warmup ops      (default 16)
  -s N      rng seed               (default clock)
  -o FILE   csv output             (default dbsweep.csv)
  -V        echo every point to stdout
```

Пунктир: сегменты по 32M читаются, по `gap = seg*(100-duty)/duty`
шагаются (lseek нисколько не стоит); сегменты стартуют эквидистантно
от LBA0, последний до края ёмкости. duty 100% = сплошной проход (HDTune
клон, время = весь диск/скорость).

Random: xorshift uniform по ВСЕЙ ёмкости, pread 4K, per-op время.
Первые -w операций - прогрев (спин-ап/кэш-обработка) в данные не идут.

Честность:
- `F_NOCACHE` + readahead off на device-fd (OS-кэш обходится)
- XOR-чексум по каждому блоку (анти-оценить оптимизацию)
- тайминг mach_absolute_time (суб-µs PPC)
- всё выравнивание на 512 (девайс-сектор); duty-шаг квантуется по
  сектору - chunk-шаг не кратный 512 ловит EINVAL на rdisk (ловили)
- EIO/ошибки чтения = строки `E,офсет,errno` в CSV - притом бонус:
  сканер поверхности, EIO = бэд-сектор.

Формат CSV (`dbsweep.csv`):
```
# dbsweep v1 dev=... capacity=...
L,<offset>,<bytes>,<latency_us>,<ok>    линейные блоки
R,<offset>,<bytes>,<latency_us>         random ops
E,<offset>,<errno>                      ошибки чтения
# lin points=... avg=...                своды в конце
```
Каждая запись csv_line делает fflush - UI тейлит файл живым.

Фокус фактуры (quicksilver-g4-867, Tiger 10.4.11, disk0 56G):
прогон smoke -m 4G duty 33 - lin 1368 pts, 32.6 MB/s, eio 0;
random 15 s - 1040 ops, median 14.3 ms, p99 21.8 ms. Первая точка
с 543 мс (спин-ап dosmastemode) и Edit=в лечение: двойной прогрев
линейной фазы + warmup random - выбросы исчезли.

## graphprobe - пробник клетки «AppKit-рисование»

Работа UI-матрицы: окно NSWindow + кастомный NSView drawRect
(NSBezierPath полилиния + NSRectFill точки осада) - рисует готовый CSV
(`/tmp/probe.csv`) в две панели. Никаких знаний про диски - только
разбор CSV + рисование, что и проверяется.

 nomомирно: Tiger 10.4.11 / G4 e7450 факт - окно живой, pololine и
2080-точек scatter рисуются, setNeedsDisplay работы - клетка закрыта.

## DiskGraph.app - боевая тулза

Окно: popup девайсов (перечень IOMedia Whole через IOKit - BSD Name и
Size из реестра), слайдер duty 10..100 + рост ETA (грубая оценка
30 MB/s до первого замера, живой ETA после), поле random-секунд,
Start / Stop, статус-строка; две панели - точки live по мере ходки.

Движок из `Contents/Resources/dbsweep` - аутенфикация паролем
(AuthorizationExecuteWithPrivileges, password-диалог).
UI stdout движка не читает: тейлит `/tmp/diskgraph.csv` таймером
0.7 с - fseek(офсет), письма до EOF, инкремент -> setNeedsDisplay.
Потоков нет вообще.

Стоп = killall dbsweep под root: движок ловит SIGTERM, аккуратно
писалает CSV - частичная ходка остаётся валидной.

## Сборка и деплой

```
rsync -a diskgraph/ intel-leopard:~/PowerTools/diskgraph/
ssh intel-leopard 'cd ~/PowerTools/diskgraph && make'
otool -hv DiskGraph.app/Contents/MacOS/diskgraph   # ppc/ALL
otool -tv ..... | grep -icE 'v(a|m)[a-z]+'         # 0 (VA нет)
md5 ...; scp -O ... tar ...; md5 same
```
Флаги - канон репо (см. Makefile), фреймворки Foundation/AppKit/
Security/IOKit. бинарь ppc/ALL мин-10.2 - Jaguar..Леопард.

## Грабли-копилка

- duty-шаг не кратный 512 → EINVAL на rdisk (ловили, 620 E за
  2G-прогон) - сделали квантование по сектору
- спин-ап даёт первую точку на 500+ мс - двойной warm-up для
  линейного и warmup -w для random (в данные не пишутся)
- Tiger `open` не знает `--args` (проглотит как путь) - пробнику
  хватает дефолта /tmp/probe.csv
- `open` передаёт приложению `-psn_...` - это не путь к CSV;
  аргумент-парсер должен пропускать `-psn*` (ловили)
- старый дубль-загрузка CSV в graphprobe: копипаст ветки else
  оставил вторую csv_load - дам счётчики 2736/2080 (2×1368/1040)
  вместо правильных (ловили, чистим)
- убывание: леопард-gcc требует `-x objective-c` для .m кросса
  (кэнон parок), IOKit umbrella на leopard нет - IOKitLib.h прямой
