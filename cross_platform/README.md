# Rug for Windows and Linux

This is a separate Python/Qt port of the macOS Rug application. It uses the original rug textures and a cloth mesh. The macOS Swift app remains in `Sources/`.

## Run

Install Python 3.10 or newer. From the repository root:

```sh
python -m venv .venv
```

On Windows:

```powershell
.venv\Scripts\python -m pip install -r cross_platform\requirements.txt
.venv\Scripts\python cross_platform\app.py
```

On Linux in an **X11** session:

```sh
.venv/bin/python -m pip install -r cross_platform/requirements.txt
.venv/bin/python cross_platform/app.py
```

To build a self-contained Windows folder, run `cross_platform/build_windows.ps1` in PowerShell. To build one on Linux, run `bash cross_platform/build_linux.sh`. PyInstaller bundles the app into `dist/Rug`; keep the whole folder together when copying it.

Drag the rug to lift it, scroll to rotate, and right-click for a menu. The settings window lets you change its size or texture. A tray icon reopens settings. Outside the rug, mouse input passes through to the desktop.

The Windows version reads Explorer's desktop icon positions for bumps. Linux desktop environments expose icon positions differently; the rug currently lies on a flat desktop there. Wayland compositors do not expose the window ordering and input shape needed for this overlay, so use an X11 session. The macOS app's icon sweeping, floor wallpaper, soft shadows, and self-collision have not yet been ported.

## По-русски

На Windows распакуй **всю** папку `Rug` из архива и запусти `Rug.exe`. Ковер находится над значками рабочего стола и под обычными окнами. Настройки открываются из значка в трее.

На Linux нужен сеанс X11. Установи зависимости командами выше и запусти `cross_platform/app.py`. Для сборки папки с исполняемым файлом запусти `bash cross_platform/build_linux.sh`. В Linux бугры от значков пока не определяются автоматически; Wayland пока не поддерживается.
