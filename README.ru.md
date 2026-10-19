<div align="center">

# Left4Craft (Left 4 Dead 2 × Minecraft)

[![English](https://img.shields.io/badge/Language-English_🇬🇧-2ea44f?style=for-the-badge)](README.md)
[![Russian](https://img.shields.io/badge/Язык-Русский_🇷🇺-007ec6?style=for-the-badge)](README.ru.md)

</div>

Играешь в Left 4 Dead 2 за Стива: настоящий Minecraft Java работает рядом скрытно и отвечает за физику
игрока, инвентарь, HUD и блоки, а L4D2 — за свой мир, заражённых, Director и кампании. Строишь из блоков
баррикады прямо на картах L4D2, и заражённым приходится их ломать.

> Порт [HL2Craft](https://github.com/ShadowBlade83) (Half-Life 2 × Minecraft, ShadowBlade83 / Maga) на
> Left 4 Dead 2.

## Что работает
- **Физика Minecraft:** ходьба, спринт, прыжок на 1,25 блока, приседание, стройка «столбом» под собой.
  Выживший L4D2 — марионетка в точке Стива (noclip, `setpos` каждый кадр).
- **Блоки внутри кадра L4D2:** меши секций Minecraft рисуются D3D9 в момент конца 3D-сцены, в буфер глубины
  игры и её же камерой (матрица `cViewProj` из констант вершинного шейдера c8..c11). Блоки прячутся за стенами.
- **HUD Minecraft вместо HUD L4D2:** рука, хотбар, сердца и экраны Minecraft поверх кадра (эффект ReShade
  `HL2Minecraft.fx`). Оружие и HUD L4D2 убираются VScript-ом.
- **Баррикады:** поставленный блок становится `env_physics_blocker` в L4D2 с запасом прочности по материалу
  (земля 50, доски 150, булыжник 300, железо 600, обсидиан 2000). Заражённые рядом его ломают, Tank — быстро,
  а сломанный блок ломается и в Minecraft.
- **Коллизия карты:** геометрия L4D2 вокруг игрока сканируется трассировками и уходит в Minecraft вокселями 1/8 блока.
- **Здоровье:** главное — у Minecraft; удары L4D2 приходят в Minecraft (÷5), здоровье выжившего зеркалит Minecraft (×5).

## Как устроено
```
Left 4 Dead 2 (32 бит, D3D9)                         Minecraft 26.3 + Fabric (fabric)
  ReShade 6.8 + HL2Minecraft.addon32 (hl2)        мод hl2craft (Left4Craft)
    мост: поза/взгляд, ввод, скан коллизии  <-- shared memory -->  физика игрока, коллизия, блоки
    рендер: меши блоков в кадр L4D2         <-- render ring  ---   экспорт секций, атласа, сущностей
    оверлей: HUD/рука Minecraft             <-- overlay      ---   кадр HUD
  VScript left4craft.nut (сервер L4D2)
    баррикады, прочность, оружие/HUD, марионетка
```
Масштаб: 1 блок = 39,37 юнита (1 м). Оси: Source (x, y, z) → Minecraft (x, z + 64, −y). Каждая карта — свой регион.

## Что нужно
- Windows 10/11.
- **Left 4 Dead 2** (Steam). Запускается с `-insecure`: VAC выключен. **Только локальная игра, никаких
  официальных серверов с модом.** Перед игрой онлайн убери `d3d9.dll`, `HL2Minecraft.addon32` и `ReShade*.ini`
  из папки игры.
- **ReShade 6.8.0 with full add-on support** в папке L4D2: `left4dead2.exe`, DirectX 9. Обычная сборка ReShade
  аддон не загружает.
- **Minecraft Java 26.3** (свой аккаунт), Fabric Loader 0.19.5+, Fabric API для 26.3; JDK 25.
- Для сборки аддона — 32-битный MinGW-w64 (`g++ -m32`, например w64devkit).

## Сборка
```bat
cd hl2
build.bat
```
Собирает `HL2Minecraft.addon32` и кладёт его в ресурсы мода. Затем:
```bat
cd fabric
gradlew.bat build
```
Готовый мод — в `fabric/build/libs/`. Сначала собери аддон: он едет внутри jar, а в git его нет.

Готовые `.jar` и `HL2Minecraft.addon32` — на странице [Releases](../../releases).

## Запуск
- **Dev:** `gradlew.bat runClient` в `fabric`. Minecraft открывает свой мир-зеркало, ставит аддон, ReShade-пресет
  и `left4craft.nut` в папку L4D2 и запускает `left4dead2.exe -insecure -condebug` сразу на `c1m1_hotel`
  (`-Dleft4craft.map` в `build.gradle`).
- **Обычно:** положи jar в `mods` инстанса Minecraft 26.3 (например, в Prism Launcher) и запусти Minecraft.
  Мод сам найдёт L4D2 через Steam, поставит файлы и запустит игру.

## Управление (в окне L4D2)
| Клавиша | Действие |
|---|---|
| W A S D, Пробел, Shift, Ctrl | движение Minecraft: ходьба, прыжок, присед, спринт |
| ЛКМ / ПКМ | удар и ломание / установка блока |
| 1–9, колесо | слоты хотбара |
| E | инвентарь Minecraft |
| Q, F, T, / | выбросить, во вторую руку, чат, команда |
| F8 | отладка: срез коллизии вокруг ног в `run.log` |

## Статус и известные проблемы
- Проверено в игре на c1m1: физика Minecraft, HUD Minecraft, оружие и HUD L4D2 убраны, блоки видны в кадре.
- Ещё не проверено в игре: урон от заражённых по Стиву (только что исправлен), блоки на камере L4D2 без дёрганья,
  прочность баррикад.
- Коллизия карты строится трассировками вокруг игрока: местами возможны застревания на ступеньках (F8 в помощь).
- Особые заражённые (Hunter, Smoker…) пока не превращаются в мобов-заглушки Minecraft, бой с ними — только L4D2.
- Только одиночная игра (с ботами) на своём сервере.

Особенности L4D2, найденные по ходу (номера методов в реальных `engine.dll`/`client.dll` отличаются от
hl2sdk-l4d2, `ClientCmd` фильтрует команды и т.п.), записаны в комментариях `hl2/src/hl2_bridge.h`.

## Структура
- `fabric/` — мод Minecraft (Fabric 26.3, Java 25): связь, хуки физики, коллизия, экспорт; в ресурсах лежат эффект
  ReShade и `left4craft.nut` (VScript внутри L4D2: баррикады, оружие/HUD, марионетка).
- `hl2/` — 32-битный аддон ReShade (C++20): мост с L4D2, скан коллизии и D3D9-рендер мира.
- `protocol/` — раскладка общей памяти для обеих сторон (`hl2craft_protocol.h`).
- `shaders/` — эффект-компоновщик ReShade `HL2Minecraft.fx`.

## Авторы и лицензии
- HL2Craft и порт — ShadowBlade83 (Maga). Код — под **CC BY-NC 4.0** (см. `LICENSE`):
  некоммерческое использование, с указанием автора.
- Идея — SkyCraft (chasmlol, Skyrim × Minecraft).
- Заголовки ReShade API (`hl2/third_party/reshade`) — crosire, BSD-3-Clause.
- Не связано с Valve, Mojang или Microsoft и не одобрено ими. Файлы игр в репозитории не хранятся.
