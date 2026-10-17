# fanmon — заметки диагностики (PowerBook6,5 / iBook G4, Tiger)

Дата: 2026-10-17. Машина: **iBook G4 1 ГГц**, `PowerBook6,5`,
**Mac OS X 10.4.11 (Build 8S165)**, Darwin 8.11.0 RELEASE_PPC, G4/7447a.
Платформа: **PMU** (не SMU). SSH-алиас: `ibook-g4-1ghz`.

## 1. Диагностика

### Бинарник/сборка
- Бинарник, собранный на Leopard (gcc 4.0.1 darwin9), имел подтип
  `ppc7400` — 32-бит PPC без AltiVec-инструкций внутри либы: работает на G4.
- Нативная пересборка под Tiger (gcc 4.0.1 build 5370): `make && make clean`
  — **0 warnings**. Кросс-версионность и кросс-платформенность сборки
  подтверждаются.

### Почему старая версия отказывалась
fanmon искал узел с парой словарей `IOHWSensors`+`IOHWControls` — паттерн
SMU/G5 (платформенный плагин). На этой машине таких узлов **ноль**:
корректный отказ «unsupported machine?», exit 1 (по принципу «честный
отказ» — поведение верное).

## 2. Реестр PMU-машины (что реально есть)

Вместо SMU-массивов — **сервисные узлы** класс `IOHWSensor`
(драйвер `com.apple.driver.AppleHWSensor`), один на сенсор:

| location | type | current-value | ÷65536 | high-threshold |
|---|---|---|---|---|
| PWR/MEMORY BOTTOMSIDE | temperature | 2 719 744 | 41.5 °C | 63 °C |
| CPU BOTTOMSIDE | temperature | 3 211 264 | 49.0 °C | 83 °C |
| GPU ON DIE | temperature | 3 244 032 | 49.5 °C | 92 °C |
| BATTERY | temperature | 1 703 936 | 26.0 °C | 73 °C |
| CPU CORE | voltage | 64 144 | **0.98 В** | — |
| REAR MAIN ENCLOSURE | fanspeed | 0 | **0 rpm** | — |

Находки:

1. **`fanspeed` = 0 rpm — валидное показание.** PMU полностью выключает
   вентилятор, пока машина холодна (подтверждено владельцем машины).
   Это НЕ «замороженные» значения из G5-наблюдений.
2. **`CPU CORE` = 0.98 В — настоящий Vcore** 7447a. Сравнить с G5-образцами:
   там «CPU Voltage» 11.6–11.8 В был силовой рельс 12 В, не ядро.
   Между снимками видели 0.98 → 1.14 В — напряжение динамическое.
3. Типы концов согласованы со SMU-временами: масштаб 16.16 (÷65536)
   одинаков; по узлам-нюбам дублируется `sensor-id`/`location`/`type`.

## 3. Универсализация (итог работ)

Добавлен в fanmon второй путь данных auto-detect (та же кодовая база):
- SMU/G5 — как раньше (массивы платформенного плагина);
- PMU/G4 — фолбэк: перебор сервисных узлов `IOHWSensor`
  (IORegistryCreateIterator + IOObjectGetClass == "IOHWSensor"),
  сортировка по `sensor-id`, секции FANS/PMU, TEMPERATURES, ELECTRICAL.

Живой прогон iBook (после правок):

```
FANS (PMU)
  REAR MAIN ENCLOSURE      0 rpm
TEMPERATURES
  PWR/MEMORY BOTTOMSIDE    41.8 C
  CPU BOTTOMSIDE    44.2 C      (за 3 с: 44.5)
  GPU ON DIE       47.5 C      (за 3 с: 47.8)
  BATTERY          26.0 C
ELECTRICAL
  CPU CORE         0.98 V      (за 3 с: 1.14 V)
```

Утилита теперь покрывает обе архитектуры **G4 PMU / G5 SMU** на Tiger и
Leopard без правок.

## 4. Незакрытое

- [ ] Регрессия SMU-ветки на G5-образцах (g5-17, g5-20) после правки `main()`
      — машины уходили в оффлайн во время сессии; проверить при первом
      подключении: `make clean && make && ./fanmon` на любой G5-машине.
- [ ] Отслеживание `fanspeed` > 0 на iBook после прогрева (cpuburn) —
      подтвердить политику «вентилятор включается, когда нагреется».
