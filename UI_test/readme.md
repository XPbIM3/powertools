# UI_test

Трей-каркас (UI в меню-баре): три ступени рецепта «как собрать тулзу
для тре», обмены живыми машинами и встречные уроки ширин/счётчиков.

- `traycpu/` — CPU% via host_statistics (Panther 10.3.9, кросс-PPC)
- `diskgauge/` — W/R MB/s via IOKit IOBlockStorageDriver + dgprobe
- `tempgauge/` — температуры via AppleSMC юзер-клиент + tmpprobe
  (leopard, натив)

Общие уроки (полные в ридмих): поправка %03d-полей против «пляски»
цифр, ширина пункта = мера 13-пт гарнитурой + запас, бандл + `open`
окно; смежные окна-разведки верхнего уровня: carbon-probe, tray-probe.
