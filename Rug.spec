# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['cross_platform/app.py'],
    pathex=[],
    binaries=[],
    datas=[('Resources', 'Resources')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
# Other programs on PATH can contribute an incompatible ICU library. QtCore
# uses the Windows ICU DLL, so keep unrelated copies out of the bundle.
a.binaries[:] = [entry for entry in a.binaries
                 if entry[0].lower() not in {"icuuc.dll", "icudt78.dll"}]
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='Rug',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='Rug',
)
