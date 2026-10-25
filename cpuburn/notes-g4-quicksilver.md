# cpuburn: PowerMac3,5 Quicksilver (G4-7450 @867) — прогрев без телеметрии

Дата: 26.10.2026 · Машина: `quicksilver-g4-867` (10.0.1.33, PowerMac3,5,
G4 7450-family/subtype 11 @867 МГц, L2 256 КБ, L3 2 МБ, 1 ГБ PC133,
Leopard 10.5.8) · Бинарь из гита, md5 `7d9f570ee2773301ccf90d22acd1f24b`
(PPC ALL, VA=0) — тот же, что верифицирован на Sawtooth.

## Прогоны

```
./cpuburn -t 5            (smoke)
cpuburn: mode=mixed threads=1 duration=5.0 s
  thread 0: busy 5010/5011 ms (100.0%)  check c8857f81f9b53e35
  wall 5.01 s, aggregate busy 5.01 thread-s, all-work-done

./cpuburn -t 60           (боевой)
cpuburn: mode=mixed threads=1 duration=60.0 s
  thread 0: busy 60000/60000 ms (100.0%)  check 321ec4214b3f1542
  wall 60.00 s, aggregate busy 60.00 thread-s, all-work-done
```

- busy 100.0 % ровно, wall = duration с точностью до 0.05 с — признаков
  троттлинга/простоя нет (на 867 МГц G4 это ожидаемо: P-стейтов нет).
- Чексуммы посчитаны и напечатаны — работа реально выполнялась; mixed =
  fp + int + mem, чек-сумма на этой машине получена впервые.
- 1 CPU → `-n` не использовался (oversubscription тут не нужен).

## Чего не хватило: температур

На этой машине **fanmon честно отказывается** — в реестре 10.5.8 нет ни
`IOHWSensor`, ни `IOHWControls`, ни `fanspeed` (подробности и разбор —
`fanmon/notes-quicksilver.md`). Поэтому прогрев шёл «слепым»: единственные
наблюдаемые симптомы — отсутствие троттлинга по busy/wall и стабильный
чексуммы при повторных запусках. Для термо-профиля нужен внешний термометр
или загрузка под другой ОС (Tiger), если там AppleHWSensor для Quicksilver
всё-таки объявлен.
