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

- **ОС:** Mac OS X 10.5.8 (Leopard), Build 9L31a
- **Архитектура:** PowerPC (Power Macintosh G5 тара, PowerMac7,3)
- **Тулчейн:** GCC 4.0.1 (powerpc-apple-darwin9-gcc-4.0.1, Apple build 5493)
- Сборка флагами: `-std=gnu99 -O2 -Wall -mcpu=G3 -Wl,-force_cpusubtype_ALL` — без warnings
- Подтип бинарника: **PPC/ALL** (как у канонических Tiger-сборок), G3-совместимость
  сохранена. Каноничное «младшее» правило (сборка на Tiger, gcc 4.0.1 darwin8)
  сохраняется как цель — вернуться на Tiger-тулчейн при появлении iBook G4 в онлайне.
  `force_cpusubtype_ALL` в Makefile вшита именно на случай сборки на Leopard-инструменте.

## Как собрать ещё раз

```sh
ssh powermac-g5
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

(C: linear ~20–43 MB/s в зависимости от режима; random 4K ~150–500 IOPS)
