# notes: PowerMac3,5 QuickSilver (G4-7450 @867), 10.4.11 Tiger

Обкатка `smartinfo`, 27.10.2026. Машина: `10.0.1.33`, `hw.model` PowerMac3,5,
`hw.cpusubtype` 11 (7450-семейство), 867 МГц.

**Важно:** машина сейчас грузится с **Tiger 10.4.11** (darwin 8.11.0),
а не с Leopard 10.5.8, как было записано при онбординге 26.10.2026.
Корень — `/dev/disk1s10`.

## Прогон

```
DEV    MODEL                      FW         SIZE   VERDICT REALLO  PEND  UNC  CRC   HOURS TEMP  NOTE
disk0  ST360021A                  3.75       55.9G  WARN         0     0    0   11   13282   37  UDMA_CRC_Error_Count = 11
disk1  ST360021A                  3.19       55.9G  WARN         0     0    0  898   41789   35  UDMA_CRC_Error_Count = 898
```

S/N: `3HR1XS3K` (disk0, fw 3.75), `3HR2E5KT` (disk1, fw 3.19).
Оба — Seagate Barracuda ATA V 60 ГБ, члены RAID-0 (см.
`diskbench/notes-quicksilver-raid0.md`).

## Что показательно

- **Поверхность обоих дисков чистая:** `5/197/198` = 0. WARN — не про
  сектора.
- **`199 UDMA_CRC_Error_Count` = 11 и 898.** У disk1 вдобавок `worst` = 1
  при текущем `value` = 200, то есть в какой-то момент канал сыпал
  непрерывно. Это уровень кабеля/контроллера, не диска: на QuickSilver
  оба винта висят на встроенном ATA/100, и 898 CRC на одном из них —
  повод проверить шлейф (80-жильный?), посадку разъёмов и что диск не
  работает в PIO/UDMA-33 из-за кабеля.
- **Пробег:** disk0 — 13 282 ч, disk1 — 41 789 ч (4.8 года). Диски
  разновозрастные, что для RAID-0 из «что было под рукой» нормально.
- **Температура:** 37 °C и 35 °C — для QuickSilver с его одним 80-мм
  вентилятором нормально.
- `off-line data collection: 0x82` — коллекция включена, простоя нет;
  self-test ни разу не запускался (`0x00`).

## Что выяснилось про API на этом стеке (10.4 PATA)

- `"SMART Capable" = Yes`, `ATA Features = 35` (0x23) — путь детекта по
  ключу, как и ожидалось для 10.4.
- В `Device Characteristics` **нет** `Serial Number` — только `Product Name`,
  `Product Revision Level` и `ATA Features`. Серийник добирается через
  `GetATAIdentifyData` (words 10-19), поэтому в утилите появился этот
  вызов под гейтом `kern.osrelease >= 7`.
- Спящий/только что проснувшийся disk0 отдаёт `kIOReturnNotReady`
  (0xe00002ed) на `GetATAIdentifyData` и первые попытки
  `SMARTReturnStatus`; помогли 6 попыток по 500 мс. До правки утилита
  показывала `no response (spun down?)` на живой машине.
- `system_profiler SPATA` на этой машине пуст (Tiger PATA его не
  заполняет) — сверять модель/прошивку пришлось через ioreg и сам
  `smartinfo -a`.

## Сборка

PPC-бинарь собран кроссом на intel-leopard (на самой машине gcc нет),
`make` + `make intel`, md5 сверен. `PPC ALL`, VA 0.
