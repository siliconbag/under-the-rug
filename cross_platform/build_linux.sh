#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m pip install -r cross_platform/requirements.txt pyinstaller
python3 -m PyInstaller --noconfirm Rug.spec
printf 'Built %s\n' "$(pwd)/dist/Rug/Rug"
