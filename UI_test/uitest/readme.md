# uitest

**Первая фактическая универсальная сборка парка**: один исходник →
два слайса → один жирный бинарь, который крутится и на PPC-Пантере,
и на intel-Leopard. Дымовой тест UI-слоя, изолированного от
hardware-слоя (ни счётчиков, ни таймеров — только каркас трея).

```
 [test]  →  клик  →  меню «Выход»  →  [NSApp terminate:]
```

## Сборка (всё на 10.5 intel хосте)

```sh
# PPC-слайс (10.2-10.4 G3/G4): канон кросс-флаги + objc1
gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
    -mmacosx-version-min=10.3 -std=gnu99 -O1 -Wall \
    -fobjc-abi-version=1 -x objective-c uitest.m -o uitest-ppc \
    -framework Foundation -framework AppKit

# i386-слайс (10.5 intel): натив
gcc -arch i386 -mmacosx-version-min=10.5 -std=gnu99 -O1 -Wall \
    -x objective-c uitest.m -o uitest-i386 \
    -framework Foundation -framework AppKit

# жирник
lipo -create -output uitest uitest-ppc uitest-i386
```

Аудит артефакта:

- ppc-слайс: **PPC ALL**, VA=0, objc1-класс `ProbeTarget` декодируется
  в легаси-форме
- `lipo -info`: architectures in the fat file: **ppc i386**
- fat md5: `4e50d6091f9208d2bb13e2e56facb6c3` (слайсы:
  ppc `8b09ada4e3beb6ace69569f24c8813c3`, i386
  `6ef793466b2afcde270f32d6ba08a624`)

`make` весь конвейер делает сам (`make all` — ppc + i386 + fat + app).

## Прогон: одна машина-матрица из одного бинаря

**Один md5 на обеих машинах** (бандл `UITest.app`, несжатый tar,
`open` по ssh, `-psn_` в ps):

| машина | ОС | слайс | итог |
|---|---|---|---|
| intel-leopard (MacBookPro3,1) | Leopard 10.5 | i386 | ✓ `[test]` виден, «Выход» погасил, ps пуст |
| powermac-g4-400 (Sawtooth) | Panther 10.3.9 | ppc | ✓ то же самое |

Юзер по итогам: «оба приложения штатно отработали — были видны и
закрылись по клику на выход». **Матрица UI-слоя: пантер-PPC-клетка и
леопард-i386-клетка закрыты одним артефактом.**

## Что это закрывает и что нет

- Закрыто: жир-пара ppc+i386 из одной codebase — механика universal
  ( Architects-два-слайса-один-файл ) рабочая, деплой-protocol тот же.
- Не закрыто (клетки матрицы остаются открытыми): 10.2-PPC (нет
  железа в парке), G5-PPC (нет железа), SMU-телеметрия. Скелет
  примет их, когда появятся.
- Не входит: hardware-слой (счётчики) — он остаётся in
  traycpu/diskgauge/tempgauge-ступенях; uitest проверяет ТОЛЬКО UI.

## Нюансы

- Ширина: константная строка, длина = мера 13-пт гарнитурой + 50 pt
  (урок двух ОС: мелкая мера недомеряет везде)
- Меню: приём traycpu — `setMenu:` на пункт с одним «Выход»
- LSUIElement: без док-иконки на обеих ОС
