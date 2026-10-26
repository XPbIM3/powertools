# smartinfo

Консольная read-only сводка SMART по всем ATA-дискам машины.
Чистый C (IOKit + CoreFoundation), без внешних зависимостей.

## Что читает

Ровно три команды на диск, обе — только чтение:

| вызов | что даёт |
|---|---|
| `SMARTReturnStatus` | общий флаг «порог превышен» |
| `SMARTReadData` | 512 байт: 30 атрибутов, статус off-line коллекции, статус последнего self-test, capability-байты |
| `SMARTReadDataThresholds` | пороги для тех же атрибутов |

**Чего не делает никогда:** `SMARTEnableDisableOperations`,
`SMARTEnableDisableAutosave`, `SMARTExecuteOffLineImmediate`,
`SMARTWriteLogAtAddress`. Тестов не запускает, состояние диска не меняет —
в коде этих вызовов просто нет.

Модель/серийник/прошивка/интерфейс берутся из IORegistry
(`Device Characteristics`, `Protocol Characteristics`), а не из ATA
IDENTIFY: так они доступны и на 10.2, где `GetATAIdentifyData` ещё нет.
IDENTIFY утилита не вызывает вообще — ABI-риска на Jaguar нет.

## Детект дисков

`/dev/diskN` (только целые диски, разделы `disk0s2` отсеиваются) →
`IOBSDNameMatching` → подъём по родителям в IOService plane. Годным
считается первый узел, у которого есть **любое** из:

- `"SMART Capable" = Yes` — штатный признак 10.4+;
- `IOUserClientClass = "ATASMARTUserClient"` — 10.2/10.3, где ключа ещё нет;
- `IOATABlockStorageDevice` + бит `0x20` в `ATA Features` (ищется и на самом
  узле, и внутри `Device Characteristics`; в SDK 10.2.8 константы
  `kIOATAFeatureSMART` нет, бит задан локально).

UUID плагина одинаков во всех SDK (10.2.8 / 10.3.9 / 10.4u / 10.5), поэтому
один и тот же код открывает:

| шина | класс user client'а |
|---|---|
| PATA | `ATASMARTUserClient` |
| SATA G5 K2 | `AppleSATLSMARTUserClient` |
| SATA AHCI | `AHCISMARTUserClient` |

CD/DVD отсеиваются по `IOCDBlockStorageDevice`; USB/FireWire-мосты и
RAID-тома попадают в таблицу строкой с причиной.

## Версии ОС

| ОС | SMART API | `SMART Capable` | IDENTIFY | примечание |
|---|---|---|---|---|
| 10.2 Jaguar | есть (10 методов) | нет | нет | детект по `ATA Features & 0x20` |
| 10.3 Panther | то же + | нет | `GetATAIdentifyData` | детект по `ATA Features` |
| 10.4 Tiger | то же | **есть** | есть | — |
| 10.5 Leopard | то же | есть | есть | + AHCI и SATL |

`GetATAIdentifyData` — последний элемент vtable, добавлен в 10.3; утилита
его не вызывает, поэтому на 10.2 структура не читается за границей.

## Вывод

```
$ sudo ./smartinfo
DEV    MODEL                      FW         SIZE   VERDICT REALLO  PEND  UNC  CRC   HOURS TEMP  NOTE
disk0  FUJITSU MHV2080BH          00000025   74.5G  OK           0     0    0    0   14391   31

1 disk: 1 OK, 0 WARN, 0 FAIL, 0 n/a
```

`-a` добавляет по каждому диску серийник, прошивку, шину и все 30 атрибутов
(`ID / NAME / FLAGS / VAL / WOR / THR / RAW`), плюс статус off-line
коллекции и последнего self-test'а — как числа из структуры, без запуска.

Флаги: `-a` полный дамп, `-d diskN` один диск, `-h` помощь.

Коды возврата: `0` всё OK, `1` есть WARN/FAIL, `2` SMART-данных не получено,
`3` ошибка аргументов.

## Вердикт

| условие | вердикт |
|---|---|
| `SMARTReturnStatus` = «порог превышен» | FAIL |
| pre-fail-атрибут (`flags & 0x0f == 0x01`) с `value <= threshold` | FAIL |
| `197 Current_Pending_Sector` ≠ 0 | FAIL |
| `5`, `198`, `199` ≠ 0 | WARN |
| иначе | OK |

Порогов нет (диск не отдал `READ THRESHOLDS`) → колонка `THR=?`, по
атрибутам вердикт не выносится.

## Атрибуты в сводке

| ID | имя | смысл |
|---|---|---|
| 5 | Reallocated_Sector_Ct | уже переназначенные сектора |
| 197 | Current_Pending_Sector | сектора, которые диск пока не смог прочитать — растёт = сыпется |
| 198 | Offline_Uncorrectable | ошибки, найденные off-line сканом |
| 199 | UDMA_CRC_Error_Count | не диск, а шлейф/контроллер (PATA) |
| 9 | Power_On_Hours | пробег |
| 194/190 | Temperature | температура |
| 193 | Load_Cycle_Count | парковки головок (ноутбуки) |

Полный список имён — в `attr_names[]`; незнакомые ID печатаются как
`Vendor_Specific`.

## Вендорские странности (проверено живьём)

- **Fujitsu MHV2080BH** (MacBookPro3,1): атрибут 9 (`Power_On_Hours`) отдаёт
  счётчик с частотой 1 Гц, а не часы. Проверено двумя чтениями с интервалом
  20 с (+21). Утилита считает это секундами, если значение > 500000
  (больше 57 лет пробега не бывает), делит на 3600 и печатает пояснение в
  `-a`.
- Тот же диск кладёт служебные данные в старшие байты `raw` (например,
  attr 5 = `00 00 00 00 d0 07`). Поэтому **RAW печатается как младшие
  32 бита** 48-битного поля: счётчики секторов и часов в 32 бита укладываются
  всегда, а верхние байты у части вендоров — мусор. Полное 48-битное поле
  дало бы `8589934592000` переназначенных секторов на здоровом диске.
- `RAW` вендор-специфичен: сравнивать между собой можно только счётчики
  `197/198/199` и `5`, остальные числа имеют смысл лишь в динамике.

## Сборка

```
make         # ppc, канон парка
make intel   # i386 (Leopard Intel), отдельная цель — не universal
```

ppc-флаги (канон `ppc-build`): `-arch ppc -mcpu=G3
-Wl,-force_cpusubtype_ALL -mmacosx-version-min=10.2`. `min=10.2` выбран
намеренно: SMART-API есть уже в Jaguar, а `min>=10.3` втачивает
`libmx.A.dylib`, которой на 10.2 нет.

Проверка после сборки: `otool -hv` → `PPC ALL`, `otool -tv | grep -ic
'v(a|m)'` → 0.

## Обкатка

Проверено живьём на **MacBookPro3,1, 10.5.8 i386** (AHCI,
`AHCISMARTUserClient`): диск определяется, атрибуты и пороги читаются,
вердикт OK, `-d`/`-a`/`-h` и коды возврата работают.

Отдельно проверено: ppc-бинарь на Intel-хосте под Rosetta **не работает** —
`IOCreatePlugInInterfaceForService` возвращает `kIOReturnNoResources`
(ppc-процесс не может загрузить i386-плагин SMARTLib). Это ожидаемо, на
Intel-машинах нужен `smartinfo-i386`; сообщение об ошибке это и говорит.

Ещё не проверено (нужны PPC-машины парка):

- 10.2 Jaguar — путь детекта без `SMART Capable` (`ATA Features` на узле
  или в `Device Characteristics`), отсутствие падения на vtable;
- 10.3 Panther — то же плюс `ATA Features` = 0x27 в фикстурах;
- G5 K2 SATA (`AppleSATLSMARTUserClient`) и PATA (`ATASMARTUserClient`);
- поведение на спящем диске и на CD в приводе.

Результаты живых прогонов писать в `notes-<модель>.md` этого каталога.
