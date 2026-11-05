$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $repoRoot
try {
    python -m pip install -r cross_platform/requirements.txt pyinstaller
    python -m PyInstaller --noconfirm Rug.spec
    $qtRuntime = Join-Path (Split-Path (python -c 'import PySide6; print(PySide6.__file__)') -Parent) 'VCRUNTIME140.dll'
    $bundleRoot = Join-Path $repoRoot 'dist/Rug/_internal'
    Copy-Item -LiteralPath $qtRuntime -Destination (Join-Path $bundleRoot 'VCRUNTIME140.dll') -Force
    Copy-Item -LiteralPath ($qtRuntime -replace 'VCRUNTIME140.dll$', 'VCRUNTIME140_1.dll') -Destination (Join-Path $bundleRoot 'VCRUNTIME140_1.dll') -Force
    Write-Output (Join-Path $repoRoot 'dist/Rug/Rug.exe')
}
finally {
    Pop-Location
}
