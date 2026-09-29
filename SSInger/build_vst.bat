@echo off
REM build_vst.bat -- rebuild the SSInger VST3 (Release) after a sound update.
REM Usage: double-click, or from the repo root:  SSInger\build_vst.bat
REM Must run from the repo checkout (this script finds the root from its own path).
setlocal EnableExtensions
cd /d "%~dp0.." || exit /b 1

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found on PATH. Install CMake 3.22+ and retry.
  exit /b 1
)

echo [1/3] Configure (reuses build\_deps, no download after the first run)...
cmake -S SSInger -B build/SSInger
if errorlevel 1 (
  echo ERROR: configure failed.
  exit /b 1
)

echo [2/3] Build VST3 (Release)...
cmake --build build/SSInger --target Robovox_VST3 --config Release
if errorlevel 1 (
  echo ERROR: VST3 build failed.
  exit /b 1
)

echo [3/3] Build + run offline C tests...
cmake --build build/SSInger --target test_6850_midi --config Release
if errorlevel 1 (
  echo ERROR: test build failed.
  exit /b 1
)
ctest --test-dir build/SSInger -C Release --output-on-failure
if errorlevel 1 (
  echo ERROR: tests failed.
  exit /b 1
)

echo.
echo OK. VST3 folder:
echo   build\SSInger\Robovox_artefacts\Release\VST3\SSInger.vst3
echo Copy that whole folder to your system's VST3 folder, then rescan in your DAW.
