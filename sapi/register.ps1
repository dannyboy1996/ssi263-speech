param([switch]$Register, [switch]$Unregister)
# Voice tokens for the SSI-263 SAPI engine: the COM class (both DLL bitnesses) and one token per voice the pipe
# server lists, in both registry views.  A trimmed fork of outspoken-nvda's sapi/register.ps1: the voices here
# come with their firmware, so there is no data root to find.
#
# PowerShell 2.0's dialect on purpose (stock Windows 7): no $PSScriptRoot, the 32-bit view written directly.
# Run elevated: -Register or -Unregister.
$ErrorActionPreference = 'Stop'
$stage = Split-Path -Parent $MyInvocation.MyCommand.Path
$clsid = '{616e7e0a-7b1a-4b94-8812-bbb597d2c025}'
$prefix = 'SSI263_'
$views = @('HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens')
if ([IntPtr]::Size -eq 8 -or $env:PROCESSOR_ARCHITEW6432) {
    $views += 'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Speech\Voices\Tokens'
}
$windir = $env:SystemRoot
# The 64-bit regsvr32 lives in System32 -- from a 32-bit PowerShell that name is redirected, Sysnative is the way in.
$sys64 = Join-Path $windir 'Sysnative\regsvr32.exe'
if (!(Test-Path $sys64)) { $sys64 = Join-Path $windir 'System32\regsvr32.exe' }
$sys32 = Join-Path $windir 'SysWOW64\regsvr32.exe'

function Remove-Voices {
    foreach ($view in $views) {
        if (!(Test-Path $view)) { continue }
        Get-ChildItem $view | Where-Object { $_.PSChildName -like "$prefix*" } | ForEach-Object {
            Remove-Item -Recurse -Force $_.PSPath
        }
    }
}

function Language-Code([string]$lang) {
    switch ($lang.Split('_')[0].Split('-')[0].ToLower()) {
        'es' { return '40A' }
        default { return '409' }
    }
}

if ($Unregister) {
    Remove-Voices
    if (Test-Path (Join-Path $stage 'x64\ssi263_sapi.dll')) { & $sys64 /s /u (Join-Path $stage 'x64\ssi263_sapi.dll') }
    if ((Test-Path $sys32) -and (Test-Path (Join-Path $stage 'x86\ssi263_sapi.dll'))) { & $sys32 /s /u (Join-Path $stage 'x86\ssi263_sapi.dll') }
    Write-Host 'SSI-263 SAPI voices removed.'
    exit 0
}
if (!$Register) { Write-Host 'Use -Register or -Unregister (elevated).'; exit 2 }

# The COM class, in each view: the x64 DLL for 64-bit programs, the x86 DLL for 32-bit ones.
$p = Start-Process -FilePath $sys64 -ArgumentList @('/s', "`"$(Join-Path $stage 'x64\ssi263_sapi.dll')`"") -Wait -PassThru
if ($p.ExitCode) { throw "regsvr32 (x64) failed: $($p.ExitCode)" }
if (Test-Path $sys32) {
    $p = Start-Process -FilePath $sys32 -ArgumentList @('/s', "`"$(Join-Path $stage 'x86\ssi263_sapi.dll')`"") -Wait -PassThru
    if ($p.ExitCode) { throw "regsvr32 (x86) failed: $($p.ExitCode)" }
} else {
    $p = Start-Process -FilePath $sys64 -ArgumentList @('/s', "`"$(Join-Path $stage 'x86\ssi263_sapi.dll')`"") -Wait -PassThru
}

# The voices, as the server lists them: "id<TAB>name<TAB>language", UTF-8.
$py = Join-Path $stage 'python\python.exe'
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $py
$psi.Arguments = "-I `"$(Join-Path $stage 'ssi_serve.py')`" --list"
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$proc = [System.Diagnostics.Process]::Start($psi)
$listing = $proc.StandardOutput.ReadToEnd()
$proc.WaitForExit()
$voices = @($listing -split "`r?`n" | Where-Object { $_ -match "`t" })
if ($voices.Count -eq 0) { throw 'the server listed no voices' }

Remove-Voices
foreach ($line in $voices) {
    $f = $line.Split("`t")
    $id, $name, $lang = $f[0], $f[1], $f[2]
    $key = $prefix + ($id -replace '[^A-Za-z0-9]', '_')
    foreach ($view in $views) {
        if (!(Test-Path $view)) { New-Item -Path $view -Force | Out-Null }
        $tok = Join-Path $view $key
        New-Item -Path $tok -Force | Out-Null
        Set-ItemProperty -Path $tok -Name '(default)' -Value $name
        Set-ItemProperty -Path $tok -Name 'CLSID' -Value $clsid
        Set-ItemProperty -Path $tok -Name 'VoiceId' -Value $id
        $attr = Join-Path $tok 'Attributes'
        New-Item -Path $attr -Force | Out-Null
        Set-ItemProperty -Path $attr -Name 'Name' -Value $name
        Set-ItemProperty -Path $attr -Name 'Gender' -Value 'Male'
        Set-ItemProperty -Path $attr -Name 'Age' -Value 'Adult'
        Set-ItemProperty -Path $attr -Name 'Language' -Value (Language-Code $lang)
        Set-ItemProperty -Path $attr -Name 'Vendor' -Value 'SSI-263 emulation'
    }
    Write-Host "registered $name ($id)"
}
