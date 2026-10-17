# Заметки: PowerBook G4 15"_HIGH (pb15-g4-leopard, 10.0.1.24)

Машина: `PowerBook5,6` — 15" PowerBook G4, **1.667 ГГц** (7447a-family,
cpusubtype 11, AltiVec есть), 1 core, 1 ГБ RAM. ОС: **Leopard 10.5.8
(Build 9L31a, Darwin 9.8.0)** — первая Leopard-машина семейства G4 в
парке (G5-шные Leopard-замеры были, G4 под Leopard — впервые).
Тулчейна на машине НЕТ — по протоколу собираем только на G4/iBook-Tiger.

ssh: алиас `pb15-g4-leopard`, ключ установлен (expect, пароль admin).

## Протокол внедрения (README): прогон G4-бинарников из гита

Всё из git Binaries (Tiger/iBook-G4 build) — **запустилось без сбоев**,
все 5 утилит smoke-прошли. Локальная сборка не потребовалась.
Это второй подтверждённый чек «G4-бинарь на чужой машине»
(первый — iBook G3/Tiger), и первый на Leopard+G4 — правило
«старая ОС → новая » сработало и на Leopard/G4.

## Находки этого внедрения

1. **scp на Leopard-sshd 5.2 требует `-O`** (legacy scp-протокол, SFTP
   метод pаспознавания путей не поднимается: `realpath ... failed`).
   Все будущие раскатки на Leopard-машины — `scp -O ...`.
2. **batinfo на этой машине ушел по smart-battery пути** — PMU-era
   `IOBatteryInfo` тут не публикуется (новее G4 стек:
   AppleSmartBattery-ключи). Наш auto-detect сработал как задуман.
   **Единицы масштаба: похоже ×10 от мА·ч** — Remaining 47868 и
   Full 54059 при дизайн-ёмкости этой батареи ~5400 мА·ч (5405.9 при
   /10), Current 4786.8 мА·ч = 88.5 %. Проверить при полном цикле;
   возможно, ×10 относятся и к Amperage (241 → 24.1 мА трюк-заряд или
   2.41 А? при проверке на цикле станет ясно).
3. **fanmon PMU-путь на Leopard-G4 работает**: 2 вентилятора
   (REAR LEFT/RIGHT EXHAUST, 0 rpm — валидный PMU-ноль, аналогично
   iBook), температуры CPU/CPU-INTREPID/PWR SUPPLY/BATT-TEMP/TRACKPAD,
   CPU CORE 1.32 В, BATT-CURRENT сенсор. Интересно: TRACKPAD-сенсор
   13.5 C — холодный пад, реальный тайп.
4. cpubench: Leopard HW_CPU_FREQ ×2³² quirk отработан (модель
   «PowerBook5,6, 1666 MHz» — правильная). FP 0.778 GFLOP/s (0.39 млрд
   FMADD/s), INT 2.55 млрд ALU ops/s — ровно ×2 против iBook G4 1.07
   ГГц на той же 7447a архитектуре (0.389 / 1.28). По тактам:
   FP ≈ 0.23 fma/cycle, INT ≈ 1.53 ops/cycle (iBook: 0.18 / 1.20) —
   15"-шасси холоднее, до конвейера доходит больше.
5. cpuburn smoke (2 с, mixed) — ок, check-сумма выдана.
6. diskbench smoke (8M/4M): linear 35.7/32.0 MB/s, random 4K
   2.5/2.6 MB/s (~1500-1600 µs/op) — обычный 4200-5400 IDE. Полный
   паспорт (128M/64M) — по мере задач.
7. Батарея: 84 цикла, дизайн ~5400 мА·ч, сейчас 88.5 % — родная
   батарея этого шасси (не перекочеванная, как у G3).

## Конфиг системы (на будущее)

- В кэше Leopard rsa-only хост-ключи — в `~/.ssh/config` локальной
  машины алиасу нужны `HostKeyAlgorithms +ssh-rsa` и
  `PubkeyAcceptedAlgorithms +ssh-rsa` (уже прописаны).
- Фоновая нагрузка при первом включении умеренная (load ~0.4-1.3,
  Spotlight не буровит на full-passport замерах).
