# fanmon

Консольный монитор вентиляторов и температур для Mac OS X Tiger/Leopard на PowerPC.
Оптимизирован под iMac G5 (PowerMac8,1), работает на любой SMU-машине.

Читает словари `IOHWControls` и `IOHWSensors` из IORegistry через публичный
IOKit API. Никаких приватных интерфейсов, чистый C.

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
