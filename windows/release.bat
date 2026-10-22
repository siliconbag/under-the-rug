@echo off
rem Builds and packs dist\Rug-windows-x64.zip (or -arm64) for GitHub Releases.
rem   release.bat         x64
rem   release.bat arm64   Windows on ARM
setlocal
cd /d "%~dp0"
set ARCH=x64
if /i "%~1"=="arm64" set ARCH=arm64
call "%~dp0build.bat" %ARCH% || exit /b 1
if not exist dist mkdir dist
if exist dist\Rug-windows-%ARCH%.zip del /q dist\Rug-windows-%ARCH%.zip
tar -a -c -f dist\Rug-windows-%ARCH%.zip -C build Rug || exit /b 1
for %%f in (dist\Rug-windows-%ARCH%.zip) do echo packed %%f, %%~zf bytes
