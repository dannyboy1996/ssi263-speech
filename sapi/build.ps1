param([string]$Stage = "")
# Stage the SSI-263 SAPI engine: both DLL bitnesses, the pipe server, the three add-ons' driver folders exactly
# as the NVDA add-ons ship them (run nvda\build_blazie.py, build_speakout.py and build_accent.py first), and the
# embeddable Python that runs the server.  Template: outspoken-nvda's sapi/build.ps1 (panthera-speech's).
#
# The stage lands in nvda\dist\sapi, beside the built add-ons: it carries the same firmware, so it is never
# committed.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (!$Stage) { $Stage = Join-Path $repo "nvda\dist\sapi" }
$msvc = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Include" -Directory | Sort-Object Name | Select-Object -Last 1
if (!$msvc -or !$sdk) { throw "MSVC Build Tools and the Windows SDK are required" }
New-Item -ItemType Directory -Force $Stage,(Join-Path $Stage "x86"),(Join-Path $Stage "x64") | Out-Null
foreach ($arch in "x86","x64") {
  $cl = Join-Path $msvc.FullName "bin\Hostx64\$arch\cl.exe"
  $out = Join-Path $Stage $arch
  & $cl /nologo /EHsc /O2 /MT /LD /DUNICODE /D_UNICODE "/I$($msvc.FullName)\include" "/I$($sdk.FullName)\um" "/I$($sdk.FullName)\shared" "/I$($sdk.FullName)\ucrt" (Join-Path $PSScriptRoot "ssi263_sapi.cpp") "/Fe$out\ssi263_sapi.dll" "/Fo$out\" /link "/DEF:$PSScriptRoot\ssi263_sapi.def" "/LIBPATH:$($msvc.FullName)\lib\$arch" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\um\$arch" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\ucrt\$arch" sapi.lib ole32.lib advapi32.lib shell32.lib
  if ($LASTEXITCODE) { throw "$arch SAPI DLL build failed ($LASTEXITCODE)" }
}
# The console-free way into the settings dialog: a GUI-subsystem launcher, so no console flashes and steals focus.
$launcherCl = Join-Path $msvc.FullName "bin\Hostx64\x64\cl.exe"
& $launcherCl /nologo /O2 /MT /W3 "/I$($msvc.FullName)\include" "/I$($sdk.FullName)\ucrt" "/I$($sdk.FullName)\um" "/I$($sdk.FullName)\shared" (Join-Path $PSScriptRoot "settings_launcher.c") "/Fe$Stage\ssi263_settings.exe" "/Fo$Stage\" /link /SUBSYSTEM:WINDOWS "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\ucrt\x64" "/LIBPATH:$($sdk.Parent.Parent.FullName)\Lib\$($sdk.Name)\um\x64" user32.lib kernel32.lib
if ($LASTEXITCODE) { throw "settings launcher build failed ($LASTEXITCODE)" }
Set-Content -Encoding ASCII (Join-Path $Stage "settings.cmd") '@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0settings.ps1"'
Copy-Item (Join-Path $PSScriptRoot "ssi_serve.py") $Stage
Copy-Item (Join-Path $PSScriptRoot "register.ps1") $Stage
Copy-Item (Join-Path $PSScriptRoot "settings.ps1") $Stage
# The drivers, fresh every time: a stage that is only ever added to keeps whatever an earlier build left in it.
$drv = Join-Path $Stage "synthDrivers"
if (Test-Path $drv) { Remove-Item -Recurse -Force $drv }
New-Item -ItemType Directory -Force $drv | Out-Null
foreach ($addon in "blazie","speakout","accent") {
  $built = Join-Path $repo "nvda\dist\$addon-build\synthDrivers"
  if (!(Test-Path $built)) { throw "$built is missing: run nvda\build_$addon.py first" }
  Copy-Item -Recurse -Force (Join-Path $built "*") $drv
}
Get-ChildItem -Recurse $drv -Directory -Filter "__pycache__" | Remove-Item -Recurse -Force
# Embeddable Python 3.8.10, deliberately: the last CPython for Windows 7, and the drivers are 3.7-compatible (NVDA
# 2021.1 shipped 3.7).  amd64 for now: 64-bit Windows, and ARM64 through its x64 emulation.
$py = Join-Path $Stage "python"
if (!(Test-Path (Join-Path $py "python38.dll"))) {
  if (Test-Path $py) { Remove-Item -Recurse -Force $py }
  $pyzip = Join-Path $env:TEMP "python-3.8.10-embed-amd64.zip"
  if (!(Test-Path $pyzip)) {
    Invoke-WebRequest -Uri "https://www.python.org/ftp/python/3.8.10/python-3.8.10-embed-amd64.zip" -OutFile $pyzip
  }
  Expand-Archive $pyzip -DestinationPath $py -Force
}
Set-Content -Encoding ASCII (Join-Path $py "python38._pth") @'
python38.zip
.
..
#import site
'@
Write-Host "SSI-263 SAPI stage: $Stage"
