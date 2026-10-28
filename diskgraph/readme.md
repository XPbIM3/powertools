# diskgraph

Дисковый бенчмарк с графиком по поверхности: raw-девайс, только чтение.

- ядра замеров `dglincore` (linear) и `dgrndcore` (random) - общий код
  для консоли и UI, наружу sink-калбеки, без stdout
- `dgdev` - общее у ядер и UI: mach-часы, открытие raw-ноды (ёмкость,
  F_NOCACHE, readahead off), перцентили; тексты ошибок нейтральные,
  подсказку про права дописывает обёртка (консоль - sudo, UI - пароль)
- консольные обёртки `dblin` / `dbrnd` (отладка, человекочитаемый stdout)
- UI `DiskGraph.app` - две вкладки Linear/Random на тех же ядрах
- движок `dbsweep` (CLI, композит + CSV, без AppKit)
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

### /dev/rdiskN против /dev/diskN - и почему F_NOCACHE не спасает

`/dev/diskN` - блочная нода: чтение идёт через кеш блоков ядра и режется
на 4K-порции, `F_NOCACHE`/`F_RDAHEAD` на неё не действуют (они для
кластерного слоя файлов). Замер на quicksilver, disk0, 256 МБ сплошняком,
pread по 1 МБ, iostat:

| нода | MB/s | KB/t (запрос к диску) | запросов/с |
|---|---|---|---|
| /dev/rdisk0 | 32.4 | 1024 | 30 |
| /dev/disk0  | 12.8 | 4    | 3200 |

На diskN меряется оверхед 3200 мелких запросов, а не поверхность.
Random 4K одинаков на обеих (14.1 / 14.5 ms) - оп и так один 4K.
В UI `/dev/diskN` оставлены для сравнения с пометкой `(buffered)`.

# dblin / dbrnd - консольные тулзы (отладочные)

Тонкие обёртки над общими ядрами замеров (`dglincore`, `dgrndcore`) -
теми же, что зовёт UI. Вся механика замера живёт в ядрах, здесь только
разбор аргументов и человекочитаемый stdout (единственный выходной
канал, CSV не нужен):

```
sudo dblin -d /dev/rdiskN [-c 32M] [-b 1M] [-D 10] [-m 0]
sudo dbrnd -d /dev/rdiskN [-B 4K]  [-T 10] [-w 16] [-s seed] [-S]
```

- dblin: пунктирный проход, чанки эквидистантно от LBA0, шаг =
  chunk*(100-duty)/duty; строка на сегмент `C ..G ..M ..MB/s blk N`
- dbrnd: -T-секундный random; строка на оп `R ..GB ..ms`;
  `-S` - режим хопов (seek), строка на хоп `S <дистанция> <+|-> <ms>`
- duty 1..100 (было 10..100)
- сводка в конце `--- chunks N blocks N avg ... eio N time m:ss`
  (у random: `ops N med ... p99 ... avg ...`)
- честность-набор как у dbsweep (raw-нода обязательна, /dev/diskN -
  warning); duty-дефолт 10%, random-дефолт 10 с (не боевые, для отладки)
- dbsweep остаётся валидированным композитом (обе фазы + CSV)

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

Окно, три вкладки (Linear read / Random access / Random seek), у каждой
свой popup устройств,
свой слайдер и свои Старт/Стоп. Вкладки независимы: свой диск, своя
ёмкость оси X, свои данные; переключение вкладок данные не теряет,
смена диска во вкладке с данными график не сдвигает (ось - по диску,
который реально читали, до нового Старта).

- селектор: пары `/dev/rdiskN` (raw) и `/dev/diskN` (buffered) рядом -
  для сравнения; ёмкость читается при выборе, поэтому ось X осмысленна
  ещё до Старта
- Linear read: ползунок duty 1..100, дефолт 5% (в простое статус
  пишет, сколько GB будет прочитано), кривая MB/s с заливкой, бежит
  слева направо, пунктир - линия avg
- Random access: 4K в равномерно случайные точки всего диска - время
  случайного доступа (seek + вращение + передача). Scatter лаг vs
  дистанция от предыдущего чтения (линейная ось GB), линии по 24
  корзинам дистанции: медиана и p10; точки выше p99 - красные
- Random seek: цепочка хопов - следующее чтение на +-d от предыдущего,
  d log-равномерно 8K..ёмкость (короткие и длинные хопы представлены
  поровну по декадам; не влезло в сторону - в другую). Scatter лаг vs
  дистанция на лог-оси (метки 4K/16K/.../64G), вперёд/назад разным
  цветом, медиана и p10 по корзинам; p10 ~ чистый seek, медиана ~ seek
  + полоборота. В статусе медианы хопов < 1 MB и > 1/2 диска.
  Живые факты quicksilver disk0 (5400 об/мин): короткие хопы вперёд и
  назад до ~100K часто из кеша диска (~0.2 ms); p10 растёт ~1 ms (1M)
  -> 3 ms (100M) -> 14 ms (полный ход); полоса на ~10.5 ms - хопы,
  опоздавшие на сектор и ждавшие полный оборот
- подсказки (тултипы) на выборе диска, слайдере, Старт/Стоп и графике
  каждой вкладки; на ярлыках вкладок нельзя - NSTabViewItem.toolTip
  только с 10.6
- единичный выброс random (рекалибровка/спин-ап) шкалу не сплющивает:
  если max > 3*p99, шкала по 2*p99, всё выше - красный треугольник у
  верхнего края и счётчик "выше шкалы: N"
- метки X - ряд 1-2-5 GB под ширину окна (не теснее ~70 px), годится
  и для 4 GB CF, и для 500 GB
- одновременно идёт только один прогон: на время прогона слайдеры,
  popup-ы и оба Старта заблокированы; Стоп активен только у идущего
- UI зовёт ядра `dglincore`/`dgrndcore` напрямую (pthread + мьютекс),
  никаких внешних процессов и разбора stdout; таймер 0.25 с
  перерисовывает графики и обновляет статус-строку. Отрисовка берёт
  копию точек под мьютексом и рисует без него - замер не ждёт кадр;
  med/p99 считаются раз в тик
- закрыли окно - приложение выходит (root-процесс не висит), ядру
  даётся до 1 с выйти из pread
- Y-шкала автоскейлится только вверх (круглые деления), внутри прогона
  не дёргается назад; X - вся ёмкость
- один прогресс в одном месте - статус-строка вкладки: `Сегмент N / M`,
  `ops N · T / T с`, в конце жирным `Готово: ... [остановлено]`,
  ошибка - красным

Права: чтение raw-ноды требует root, поэтому при запуске приложение
само поднимает права - `AuthorizationExecuteWithPrivileges`, пароль
спрашивает системный SecurityAgent ("diskgraph requires that you type
your password"). Отдельный `sudo` не нужен, движок в бандле не лежит.
Если диалог показать нельзя (запуск из ssh-сессии без GUI), приложение
работает без прав и честно пишет об этом в статус-строке.

Dev-хуки (для проверки без рук, аргументы приложения):

```
-lin        сразу стартовать Linear read
-acc        сразу стартовать Random access
-seek       сразу стартовать Random seek
-tab N      открыть вкладку N (0..2)
-cycle      стартовать linear и щёлкать вкладки, печатая счётчики
-stop N     через N секунд нажать "Стоп" (проверка cancel)
-again      после завершения прогона стартовать снова
-close N    через N секунд закрыть окно (проверка выхода)
-ldev PATH  диск вкладки (-adev - Random access, -sdev - Random seek)
-devs       напечатать список дисков и выйти
-w N -h N   стартовый размер окна
```

Плюс env-переменные: `DISKGRAPH_AUTH_USER`/`DISKGRAPH_AUTH_PASS` -
подставить учётные данные вместо диалога (автотесты);
`DISKGRAPH_NOELEVATE=1` - не поднимать права (проверка пути EACCES).

## Бэклог: тестовый UI-билд 29.10.2026

Бинари `dblin`, `dbrnd`, `diskgraph` (= `DiskGraph.app/Contents/MacOS/
diskgraph`) в этом коммите собраны НЕ по канону intel-leopard, а
нативно на одной машине - quicksilver-g4-867 (Tiger 10.4.11, PowerMac3,5
G4 867), родным `powerpc-apple-darwin8-gcc-4.0.1` (build 5370), тем же
Makefile и теми же флагами (`-arch ppc -mcpu=G3
-force_cpusubtype_ALL -mmacosx-version-min=10.2`):

```
scp -O *.c *.h *.m Makefile quicksilver-g4-867:PowerTools/diskgraph/
ssh quicksilver-g4-867 'cd ~/PowerTools/diskgraph && make dblin dbrnd DiskGraph.app'
```

Аудит: 0 warnings, `otool -hv` - PPC ALL, VA-инструкций 0.
md5: dblin 4fecb371..., dbrnd 8756f562..., diskgraph 28d48ad2...

Проверено на живой машине (root через sudo, dev-хуки): полный linear
disk0 duty 10% - 179 сегм., avg 28.8 MB/s, 3:34; random 30 с на rdisk1 -
1720 ops, med 16.1 ms, p99 38.0 ms, eio 0; -cycle/-stop/-close, путь
без прав (NOELEVATE), перезапуск под root через AEWP. На Jaguar/Panther/
Leopard этот билд не гонялся - перед релизом пересобрать по канону
intel-leopard и пройти smoke по парку.

`DiskGraph.app/Contents/Resources/dbsweep` - остаток старой схемы,
новый UI его не зовёт (ядра в процессе); можно убрать из бандла.

Открыто (точность замера, сознательно отложено):
- linear: первый блок сегмента включает seek после skip - MB/s
  занижены на ~1-3% (больше при малом duty); читать первый блок без
  тайминга
- random: `avg_us = t_io/ln` - в t_io попадают и ошибочные опы
- random: длительность по `time()` (шаг 1 с) - перевести на mach-время
- random: EIO прогрева идут в `eio`, но не в `on_err`
- XOR-цикл по буферу не нужен как анти-оптимизатор (pread не
  выкидывается) - лишь грузит CPU G3/G4 между блоками

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

## authprobe - верификатор Authorization-вызова

`authprobe.c` - малый CLI-пробник той же механики, что в DiskGraph:
AuthorizationCreate + ExecuteWithPrivileges(/bin/sh -c 'echo ok >
/tmp/authprobe.out'), печатает каждый OSStatus поимённо и факт работы
root-тула. Живёт в репо для будущих клеток/Tiger-машин:

```
rsync -a authprobe.c intel-leopard:/tmp/ && ssh intel-leopard \
'gcc -std=gnu99 -O2 -Wall -arch ppc -mcpu=G3 \
 -Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2 \
 -framework Security -o /tmp/authprobe2 /tmp/authprobe.c' && \
scp -O intel-leopard:/tmp/authprobe2 МАШИНА:/tmp/
# на МАШИНЕ в Terminal: /tmp/authprobe2  (ввести пароль в диалог)
```

## Грабли-копилка

- **AuthorizationExecuteWithPrivileges на Tiger: options MUST be zero.**
  Заголовок 10.4u SDK прямо: "Option bits (reserved). Must be zero."
  Наш первый вызов с bundled InteractionAllowed|ExtendRights дал
  -60011 = errAuthorizationInvalidFlags (не диалог, а честный отказ).
  Правка: kAuthorizationFlagDefaults - диалог SecurityAgent рисуется
  сам, флагов не надо (подтверждено authprobe-пробником на живой
  машине: root-тул отработал).
- **Диалог пароля не показывается из ssh-сессии.** Запуск
  DiskGraph.app по ssh (без sudo) даёт `authorization failed
  (-2147417888 / 0x800100e0)`: сессия ssh не та GUI-сессия, где живёт
  SecurityAgent. Из Finder (двойной клик) диалог рисуется нормально.
  Приложение при этом не падает - работает без прав и пишет в статус
  «нет прав на raw-диск».
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
