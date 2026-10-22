# Rug

Sweep your Mac desktop under the rug. A heavy wool rug lies on top of your desktop icons, the files make bumps in it, and you can grab any corner to peek at the mess underneath.

[Русская версия ниже](#по-русски)

![Rug demo](docs/demo.gif)

Thanks to [Terkel](https://x.com/terkelg) for the inspiration.

## Install

1. Download `Rug.zip` from [Releases](../../releases/latest), unzip it and move `Rug.app` to Applications.
2. Open it. The app is not notarized, so the first time macOS refuses to open it. Go to System Settings, Privacy & Security, scroll down and click **Open Anyway**. Or run this once in Terminal.

   ```bash
   xattr -dr com.apple.quarantine /Applications/Rug.app
   ```

3. Allow **Accessibility** when asked. Rug reads where Finder put your icons and stacks, so the rug gets bumps in the right places. It does not click or type anything.

macOS 14 or later, Apple Silicon or Intel.

On Windows? There's a Windows 10 and 11 version in [windows](windows/README.md), with the same rugs and physics.

## Use

- Drag the rug by any edge. Lift a corner, fold it over, flip the whole thing.
- Scroll over the rug to rotate it. Right-click it for a quick menu.
- The settings window opens on launch and whenever you open Rug again from Applications or Spotlight. Pick a rug, a floor wallpaper and the size there, or quit the app.
- **Sweep under the rug** piles all desktop icons in a heap under the rug and drops the rug on top. **Put them back** restores the old layout. This needs Finder to stop arranging icons by itself. Right-click the desktop, uncheck **Use Stacks** and set **Sort By** to **None**. Rug asks Finder for permission (Automation) the first time.
- **Your image** turns any top-down picture into a rug. It works best when the rug fills the whole frame.

Outside the rug every click goes straight to the desktop. When nothing moves, Rug stops rendering and uses next to no CPU.

## Rugs and floors

![Rugs](docs/rugs.jpg)

Four rugs (Persian, Neural with circuit-board ornaments, Soviet, Pixel) and three floors (Soviet herringbone parquet, Soviet mosaic parquet, oak planks). All textures were generated with Nano Banana, the prompts are in [tools/PROMPTS.md](tools/PROMPTS.md).

## Build from source

You only need the Command Line Tools, no Xcode project.

```bash
xcode-select --install
```

```bash
./build.sh && open build/Rug.app
```

`./release.sh` builds a universal app and packs `dist/Rug.zip`. Set `RUG_SIGN` to your certificate name if you want macOS to keep permissions between rebuilds.

`Rug --demo <folder> [rug image]` runs the scene offscreen and saves frames, handy for tuning the physics without touching your desktop. `RUG_SCENARIO=flip`, `curl` or `readme` pick other scenes, `RUG_COLS`, `RUG_BEND`, `RUG_G` and friends override cloth parameters (see `ClothParams` in `Sources/Cloth.swift`).

## How it works

- A transparent borderless window sits one level above the desktop icons and below all other windows, on every Space. It ignores the mouse everywhere except over the rug, the hit test runs against the projected cloth mesh.
- The cloth is XPBD with substeps, long-range tethers from the grabbed point so it never stretches like rubber, and self-collisions through a spatial hash. The approach follows [Ten Minute Physics](https://matthias-research.github.io/pages/tenMinutePhysics/) by Matthias Müller.
- The floor has Coulomb friction and a height map. Icon bumps come from the Accessibility tree of Finder's desktop, stacks get taller bumps.
- Rendering is Metal. A slight perspective camera, soft PCSS shadows from a height map seen from the light, the muted back side of the rug, wool relief from the texture.

## License

MIT. Made by Artem from the Telegram channel [Силиконовый мешок](https://t.me/prompt_design).

---

## По-русски

Замети рабочий стол под ковер. Тяжелый шерстяной ковер лежит поверх иконок на Mac, файлы под ним торчат буграми, а за любой угол можно взяться и заглянуть, какой там бардак.

Спасибо [Terkel](https://x.com/terkelg) за вдохновение.

### Установка

1. Скачай `Rug.zip` из [Releases](../../releases/latest), распакуй и перенеси `Rug.app` в «Программы».
2. Открой. Приложение не нотаризовано, поэтому в первый раз macOS его не пустит. Зайди в Системные настройки, «Конфиденциальность и безопасность», пролистай вниз и нажми **«Все равно открыть»**. Или выполни один раз в Терминале эту команду.

   ```bash
   xattr -dr com.apple.quarantine /Applications/Rug.app
   ```

3. Разреши **Универсальный доступ**, когда спросит. Так Ковер видит, где Finder разложил иконки и стопки, и бугры встают на свои места. Ничего не нажимает и не печатает.

Нужна macOS 14 или новее, Apple Silicon или Intel.

Для Windows 10 и 11 есть своя версия в папке [windows](windows/README.md), с теми же коврами и физикой.

### Как пользоваться

- Тяни ковер мышью за любой край. Можно поднять угол, загнуть, перевернуть целиком.
- Прокрутка над ковром поворачивает его, правый клик открывает короткое меню.
- Окно настроек открывается при запуске и каждый раз, когда снова открываешь Ковер из «Программ» или Spotlight. Там выбор ковра, пола на обоях, размер и выход.
- **«Замести под ковер»** сваливает все иконки в кучу под ковром и роняет ковер сверху. **«Разложить обратно»** возвращает их на места. Для этого Finder не должен раскладывать иконки сам. Правый клик по рабочему столу, снять **«Использовать стопки»**, в **«Сортировать по»** выбрать **«Нет»**. В первый раз Ковер попросит разрешение управлять Finder.
- **«Своя картинка»** превращает в ковер любую картинку с видом сверху. Лучше всего, когда ковер занимает весь кадр.

Мимо ковра все клики идут прямо на рабочий стол. Когда ничего не двигается, Ковер перестает рисовать и почти не ест процессор.

### Ковры и полы

Четыре ковра (персидский, нейроковер с узором из микросхем, советский, пиксельный) и три пола (советская елочка, советская мозаика, дубовые доски). Все текстуры сгенерированы в Nano Banana, промпты лежат в [tools/PROMPTS.md](tools/PROMPTS.md).

### Сборка из исходников

Хватит Command Line Tools, Xcode не нужен. Команды те же, что в английской части выше. `./build.sh` собирает под твою машину, `./release.sh` делает универсальную сборку и `dist/Rug.zip`.

Режим `Rug --demo <папка> [картинка ковра]` прогоняет сцену без окна и сохраняет кадры. Удобно подбирать физику, не трогая рабочий стол.

### Лицензия

MIT. Сделал Артем, канал [Силиконовый мешок](https://t.me/prompt_design).
