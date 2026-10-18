# Заметки: MacBook Pro 3,1 Intel (intel-leopard, 10.0.1.25)

Исследовательский визит (машина под сборкой другого проекта — только
read-only ioreg, ничего не ставили и не меняли). ОС: Leopard 10.5.8
(9L31a), Darwin 9.8.0 **i386** — по сути та же 10.5-платформа, что у
pb15-g4-leopard, но граф и термический стек совсем другие: ACPI+SMC
вместо OF/PMU. Наши PPC-бинарники на этой машине **не исполняются**
(i386) — визит чисто реестровый.

## Термический граф (в отличие от PMU/SMU на PPC)

1. **`ACPI_SMC_PlatformPlugin` (IOPlatformPlugin)** публикует словари
   **`IOHWSensors` / `IOHWControls`** — тот же формат, что SMU-путь
   fanmon на G5! find_owner-логика fanmon нашла бы этот узел оба
   (там есть оба массива). Но содержимое:
   - `IOHWSensors` — **один сенсор**: `location="GPU"`, `type="gpu"`
     (не "temp" — фильтр TEMPERATURES fanmon его пропустит),
     `current-value` = 6553600 → 16.16 = **100.0**; на сервисном узле
     `IOHWSensor` (AppleHWSensor, IOPropertyMatch device_type=gpu-sensor)
     live-значение уже **6422528 → 98.0** — данные живые, между двумя
     снимками плывут на ~2°. 98-100 °C для 8600M GT под компил-нагрузкой
     — правдоподобно (известное страдание поколения), но калибровка
     масштаба 16.16 для type="gpu" — открытая проверка.
   - `IOHWControls` — **5 лимитных контролей** (AppleSMCControl,
     type="smc"): cpu/gpu/mem plimit + cpu/gpu state. Это throttle-
     лимиты, НЕ вентиляторы — `fan-rpm`-контролей в графе нет.
   - `IOHWCtrlLoops` — PID-мета-циклы (Normal/Reduced Power/...),
     `IOEnvironment` (external-overtemp, chassis-switch и т.п.).
2. **Вентиляторы и настоящие температуры живут внутри `AppleSMC`**
   (IOUserClientClass = AppleSMCClient): SMC-ключи (TC0D и пр.) в
   реестр не выкладываются — полуприватная SMC-таблица через
   user-client (по духу то же, что ApplePMUUserClient, но с известными
   community ключами). Использование — отдельное решение (пока не делаем).
3. `AppleLMUController`/`AppleSMCFamily` — ambient-light (не thermal).
4. Корень графа — **AppleACPIPlatformExpert** (ACPI-иерархия), а не
   device-tree платформы; узлов с device_type=battery/PMU нет вообще.

## Батарея: на Intel-стеке ПУБЛИЧНО почти всё

`AppleSmartBattery` (Leopard x86) отдаёт в реестре:
- CurrentCapacity/MaxCapacity/DesignCapacity = 2377/2377/5200 мА·ч —
  **обычные мА·ч без всякого ×10** (на pb15 было 54059: там масштаб
  ×10 был pb-специфичный или стек-специфичный — сравнение двух
  Leopard-машин показывает, что единицы не жёстко фиксированы);
- **`CellVoltage` = (3909, 4133, 4132, 0)** — по-ячеечные напряжения!
  Сумма 12174 = Voltage. 3S-пак + пустой 4-й слот. То, что на G3/G4
  недоступно публично, на Intel-стеке лежит в открытом реестре;
- **`BatterySerialNumber`** = "SMP-ASMB012-3eeb-5ee" — есть;
- `Temperature` = 3002 → 30.02 °C (десикальвины ÷100) — правдоподобно;
- `LegacyBatteryInfo` — PMU-формат словаря (Capacity/Voltage/Flags=5...).
- Износ: health = 2377/5200 = 45.7 %, cycle count 38 — деградировавший
  образец (хороший тест-кейс для batinfo-подсчётов health).

## Что это значит для наших утилит (код НЕ менялся — только insight)

- batinfo smart-battery ветка изначально спроектирована под этот стек;
  на Intel-машине она бы сработала и выдала бы ГОРАЗДО больше: per-cell
  + серийник + температура. На PPC (PMU-era) это недостижимо публично.
- Формат IOHWSensors/IOHWControls — ПЕРЕКРЕСТОК между G5-SMU и Intel
  ACPI-SMC: тот же формат, но семантика полей (type="gpu", лимитные
  контроли) другая. Если появится task «термо-мониторинг на Intel» —
  фундамент уже есть, всё публично, кроме фан-RPM (только через
  AppleSMCClient).

## Обстоятельства визита

- ssh: алиас `intel-leopard`, ключ установлен, вход работает;
- PPC-бинарники на ней не запускаются по архитектуре — собирать/гонять
  нечего, это research-машина реестра;
- фоновая нагрузка ~2.2 под чужую сборку — замеры не проводили.
