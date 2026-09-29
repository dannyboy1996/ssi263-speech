@echo off
REM build_vst.bat -- rebuild the Robovox VST3 (Release) after a sound update.
REM Usage: double-click, or from the repo root:  robovox\build_vst.bat
REM Must run from the repo checkout (this script finds the root from its own path).
setlocal EnableExtensions
cd /d "%~dp0.." || exit /b 1

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found on PATH. Install CMake 3.22+ and retry.
  exit /b 1
)

echo [1/3] Configure (reuses build\_deps, no download after the first run)...
cmake -S robovox -B build/robovox
if errorlevel 1 (
  echo ERROR: configure failed.
  exit /b 1
)

echo [2/3] Build VST3 (Release)...
cmake --build build/robovox --target Robovox_VST3 --config Release
if errorlevel 1 (
  echo ERROR: VST3 build failed.
  exit /b 1
)

echo [3/3] Build + run offline C tests...
cmake --build build/robovox --target test_6850_midi --config Release
if errorlevel 1 (
  echo ERROR: test build failed.
  exit /b 1
)
ctest --test-dir build/robovox -C Release --output-on-failure
if errorlevel 1 (
  echo ERROR: tests failed.
  exit /b 1
)

echo.
echo OK. VST3 folder:
echo   build\robovox\Robovox_artefacts\Release\VST3\Robovox.vst3
echo Copy that whole folder to "C:\Program Files\Common Files\VST3\" then rescan in your DAW.
