# carbon-probe

Разведка боем тулчейна: **можно ли собирать десктопные GUI-приложения для
Mac OS X 10.3.9 (Panther) на intel-хосте Leopard. Ответ (24.10.2026): ДА.**

## Что это

Один документ-window с единственной кнопкой **«выход»**: клик — приложение
закрывается (красный бокс окна заведён туда же). Пробник показывает: окно
рисуется, кнопка работает, русские надписи рендерятся, событийный цикл
чисто стартует и чисто гасит процесс.

## Рецепт (разведка подтверждена запуском на живой машине)

- **Сборка** (кросс на intel-leopard, MacBookPro3,1 10.5.8):

```sh
gcc -arch ppc -mcpu=G3 -Wl,-force_cpusubtype_ALL \
    -mmacosx-version-min=10.3 -std=gnu99 -O1 -Wall \
    probe.c -o probe -framework Carbon
```

- **Аудит бинаря:** `otool -hv` → **PPC ALL**; `otool -tv | grep -icE 'v(a|m)[a-z]+'`
  → **0**. Артефакт md5 `5397000c874e9d49a339f510e9b0f0cc`.
- **Деплой:** бандл `Probe.app` (Info.plist + MacOS/probe), `tar` (несжатый —
  на чистой Пантере нет /usr/bin/gzip), `scp -O`, распаковка в `~/`.
- **Запуск по ssh — через `open`:** `open ~/Probe.app`. Это критично: процесс
  из-под sshd не в консольной сессии, голый бинарь не достучится до
  WindowServer; `open` отдаёт его LaunchServices, и тот поднимает бандл в
  консольной сессии (виден суффикс `-psn_…` в `ps`). Окно появляется на
  подключённом дисплее.
- **Проверено:** запуск → окно на экране → закрытие по кнопке и по
  красному боксу (PowerMac3,1, Sawtooth G4-7400 @400, Panther 10.3.9).
  Процесс гаснет в связке `QuitApplicationEventLoop` (в `ps` чисто).

## Дисциплина API (штамп для будущих GUI-инструментов парка)

Только Panther-эпоха, всё с 10.0–10.2 и живое в 10.3.9:

| API | с версии |
|---|---|
| `CreateNewWindow` + `kWindowStandardHandlerAttribute` | 10.0 / 10.2 |
| `CreatePushButtonControl`, `SetWindowTitleWithCFString` | 10.0 |
| `InstallControlEventHandler`, `kEventControlHit` | 10.1 / 10.2 |
| `RunApplicationEventLoop`, `QuitApplicationEventLoop` | 10.0 |

- **Кириллица без флагов-рисков:** `CFStringCreateWithCString(...,
  "текст", kCFStringEncodingUTF8)` — не полагаться на `CFSTR()`/константные
  строки (лишний флаг на старом gcc — риск).
- **Не NIB:** весь UI строится кодом, Interface Builder не нужен вообще —
  дистанционная сборка остаётся одиночным .c по канону репо.
- Leopard-хедеры молча дадут скомпилировать API новее 10.3 → рантайм-падение
  на машине. Правило: каждый вызов проверять «живёт ли это в 10.2/10.3».
- Cocoa/objc путь безполезен на дистанции (objc1 + nib генерить нечем) —
  Carbon программы — канон GUI парка.

## Как собрать ещё раз

```sh
# кросс (канон): слить папку на intel-leopard, там:
make            # = probe + Probe.app
# нативно на PPC-машине парка тоже:
# ssh <ppc-машина>; cd ~/Projects/carbon-probe; make
```

Запуск на целевой машине: `scp -O` папки, на Пантере `make app` не нужен,
бандл уже готов: `open Probe.app` (локально) или `open ~/Probe.app` по ssh.
