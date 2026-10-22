@echo off
rem Builds build\Rug\Rug.exe with MSVC. No Visual Studio project needed, only the C++ build tools
rem ("Desktop development with C++" in Visual Studio or the Build Tools for Visual Studio).
rem   build.bat         x64
rem   build.bat arm64   for Windows on ARM
setlocal
cd /d "%~dp0"
set ARCH=x64
if /i "%~1"=="arm64" set ARCH=arm64

if defined VSCMD_ARG_TGT_ARCH if /i "%VSCMD_ARG_TGT_ARCH%"=="%ARCH%" goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
set VCARCH=amd64
if /i "%ARCH%"=="arm64" set VCARCH=amd64_arm64
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %VCARCH% >nul 2>nul
if errorlevel 1 goto novs

:build
if exist build\Rug rmdir /s /q build\Rug
if not exist build\obj mkdir build\obj
mkdir build\Rug

pushd src
rc /nologo /fo ..\build\obj\Rug.res Rug.rc
popd
if errorlevel 1 exit /b 1

rem /MT links the C runtime statically, so Rug.exe runs without the VC++ redistributable.
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Fobuild\obj\ src\*.cpp build\obj\Rug.res /Febuild\Rug\Rug.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO
if errorlevel 1 exit /b 1

rem rugs and floors are shared with the Mac app, in Resources at the top of the repo
xcopy /e /i /q /y ..\Resources\rugs build\Rug\Resources\rugs >nul
xcopy /e /i /q /y ..\Resources\floors build\Rug\Resources\floors >nul
echo built build\Rug\Rug.exe
exit /b 0

:novs
echo Visual Studio C++ build tools not found. Install "Desktop development with C++" from
echo https://visualstudio.microsoft.com/downloads/ (Build Tools for Visual Studio are enough).
exit /b 1
