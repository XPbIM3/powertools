# diskbench

Консольная утилита для измерения скорости диска (линейное и случайное чтение/запись).
Написана на чистом C (POSIX: `open/read/write/pread/pwrite/fsync` + `F_NOCACHE/F_FULLFSYNC`), без внешних зависимостей.

## Что измеряет

| Фаза            | Механика                                              |
|-----------------|-------------------------------------------------------|
| linear write    | последовательно, затем `F_FULLFSYNC` (fallback `fsync`)|
| linear read     | чтение только что записанного файла                   |
| random write    | `pwrite` по случайным смещениям, затем `F_FULLFSYNC`  |
| random read     | `pread` по тем же случайным смещениям                 |

По умолчанию работает **без кэша ОС** (`F_NOCACHE`, шапка показывает `[nocache]`)
— цифры получаются честными. Передать `-c`, если нужен режим с кэшем (`[os cache]`).

## Параметры

```
./diskbench [-s SIZE] [-r SIZE] [-p PATH] [-b SIZE] [-B SIZE] [-c] [-k] [-h]
  -s SIZE   размер чанка для линейных тестов   (default 512M)
  -r SIZE   размер чанка для случайных тестов  (default 16M)
  -p PATH   путь к чанк-файлу                  (default ./diskbench_chunk.bin)
  -b SIZE   блок линейного I/O                 (default 4M)
  -B SIZE   блок случайного I/O                (default 4K)
  -c        разрешить кэш ОС (F_NOCACHE выключить)
  -k        не удалять чанк-файлы после теста
  -h        справка
```

Размеры: `K`, `M`, `G` или просто байты. Чанк по умолчанию создаётся в текущей
каталоге и после теста удаляется.

## Использованная среда сборки

- **Канон пересборки** (23.10.2026, тот же рецепт, что у pimem): кросс-сборка на
  **intel-leopard** (`i686-apple-darwin9-gcc-4.0.1`, build 5493), ключи-флаги:

```sh
gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
    -mmacosx-version-min=10.3 -std=gnu99 -O2 -Wall diskbench.c -o diskbench
```

- `-arch ppc` — кросс-цель; `-mcpu=G3` — набор инструкций 750 (G3-safe слой 1);
  `-Wl,-force_cpusubtype_ALL` — подтип Mach-O **PPC/ALL** (слой 2, одинаковый
  бинарь на G3/G4/G5); `-mmacosx-version-min=10.3` — переносимость на
  Panther/Tiger dyld (кросс с Leopard без minos тянет символы/libc,
  которых нет на старых системах — ловили на pimem: `___stderrp`).
- Аудит после сборки: `otool -hv` → PPC ALL; `otool -tv | grep -icE 'v(a|m)[a-z]+'` → **0**.
  Артефакт: md5 `5befe44abda4e420a34cb189be5c37a9`.
- Верификация запуском: PowerMac3,1 (Sawtooth G4-7400 @400, Tiger 10.4.11,
  онбординг 23.10.2026) — полный дефолтный прогон прошёл, цифры ниже.
- Родная PPC-сборка (`make` на любой PPC-машине парка) остаётся валидной;
  Makefile держит оба слоя `G3-safe` по умолчанию.

## Как собрать ещё раз

```sh
# кросс (канон): слить исходник на intel-leopard и собрать флагами выше
# либо нативно на любой PPC-машине парка:
ssh <ppc-машина>
cd ~/Projects/diskbench
make clean && make
```

## Пример результата (iMac G5, SATA-1 HDD, nocache)

```
linear write       6.236 s      20.5 MB/s
linear read        5.965 s      21.5 MB/s
random write      62.492 s       1.0 MB/s        262 ops/s  avg  3814.2 us/op
random read      111.934 s       0.6 MB/s        146 ops/s  avg  6831.9 us/op
```

## Пример результата (PowerMac3,1 Sawtooth G4-7400@400, IDE-диск, nocache, 23.10.2026)

```
linear write      16.846 s      30.4 MB/s
linear read       26.115 s      19.6 MB/s
random write      31.639 s       0.5 MB/s         129 ops/s  avg  7724.4 us/op
random read       36.362 s       0.4 MB/s         113 ops/s  avg  8877.5 us/op
```

(C: linear ~20–43 MB/s в зависимости от режима; random 4K ~150–500 IOPS)
