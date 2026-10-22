# Builds windows/src/Rug.ico from Resources/icon_1024.png (needs Pillow). Run once after changing the icon.
#   python windows/tools/make_ico.py
from pathlib import Path
from PIL import Image

windows = Path(__file__).resolve().parent.parent
img = Image.open(windows.parent / "Resources" / "icon_1024.png").convert("RGBA")
img.save(windows / "src" / "Rug.ico", sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)])
print("wrote", windows / "src" / "Rug.ico")
