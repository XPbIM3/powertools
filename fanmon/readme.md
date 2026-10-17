# fanmon

Консольный монитор вентиляторов, температур и электрики для Mac OS X
Tiger/Leopard на PowerPC **G4/G5**. Одна кодовая база, два пути данных,
автоматическое определение платформы; только публичный IOKit API.

## Пути данных (auto-detect)

- **SMU (G5)** — iMac G5 / Power Mac G5: читает словари `IOHWSensors` и
  `IOHWControls` платформенного плагина (узлы `SMU_*_PlatformPlugin`).
- **PMU (G4)** — iBook/PowerBook G4, Mac mini G4, Power Mac G4: словарей
  нет, берутся по одному сервисными узлы `IOHWSensor` (драйвер
  `AppleHWSensor`), те же масштабы 16.16 (÷65536).

Особенности данных: на SMU-машинах обороты берутся из `target-value`
(реестровый `current-value` «заморожен» — см. `notes-*.md`); на PMU-машинах
`fanspeed` = 0 rpm — **валидное** показание: PMU полностью выключает
вентилятор, пока машина холодна (iBook G4).

## Вывод

- **FANS** — RPM всех вентиляторов; диапазон min..max. Числом оборотов считается
  `target-value` из реестра — единственное «живое» значение (верифицировано
  на слух на iMac G5: высота шума вентилятора следует за target). Реестровый
  `current-value` на SMU-машинах «заморожен» на минимуме и не отображается
  (см. `notes-*.md`).
- **TEMPERATURES** — температуры (CPU-диод, жесткий диск)
- **ELECTRICAL** — потребление CPU (Вт, А, В)

Масштаб значений: 16.16 fixed point (`÷65536`) для температурных/электрических
сенсоров — подтверждено внутренним PID-профилем машины (target HD temp = 58.0 °C,
power-max = 45 Вт = TDP PowerPC G5). Вентиляторы хранятся как целые RPM.

## Запуск

```
./fanmon            # разовый вывод
./fanmon -w 2       # режим наблюдения, обновление каждые 2 с
```

## Сборка

- **ОС:** Mac OS X 10.4.11 (Tiger), iMac G5 PowerPC
- **Тулчейн:** Xcode 2.5 → GCC 4.0.1 (powerpc-apple-darwin8)
- Флаги г. `-std=gnu99 -O2 -Wall`, линкер с `-framework CoreFoundation -framework IOKit`

```sh
ssh tiger
cd ~/Projects/fanmon
make clean && make
```
