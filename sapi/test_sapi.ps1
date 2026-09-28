# End to end through SAPI itself: every registered SSI-263 voice speaks "Hello, how are you??" with a bookmark in the
# middle, into a wav; the audio must be speech-length and voiced, and the bookmark event must arrive (NVDA's SAPI
# driver waits for them -- an engine that never sends one stalls NVDA).  Run from 64-bit PowerShell for the x64
# DLL, from %windir%\SysWOW64\WindowsPowerShell\v1.0\powershell.exe for the x86 one.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File sapi\test_sapi.ps1 [-OutDir <folder for the wavs>]
param([string]$OutDir = $env:TEMP)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$bits = if ([IntPtr]::Size -eq 8) { '64-bit' } else { '32-bit' }
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$ours = @($synth.GetInstalledVoices() | Where-Object { $_.VoiceInfo.AdditionalInfo['Vendor'] -eq 'SSI-263 emulation' })
if ($ours.Count -eq 0) { Write-Host "FAIL ($bits): no SSI-263 voices registered"; exit 1 }
$bad = 0
foreach ($v in $ours) {
    $name = $v.VoiceInfo.Name
    $s = New-Object System.Speech.Synthesis.SpeechSynthesizer
    $s.SelectVoice($name)
    $wav = Join-Path $OutDir ("sapi_" + ($name -replace '[^A-Za-z0-9]', '_') + "_$bits.wav")
    $s.SetOutputToWaveFile($wav)
    $pb = New-Object System.Speech.Synthesis.PromptBuilder
    if ($v.VoiceInfo.Culture.TwoLetterISOLanguageName -eq 'es') {
        $pb.AppendText('Hola,'); $pb.AppendBookmark('7'); $pb.AppendText('¿cómo estás?')
    } else {
        $pb.AppendText('Hello,'); $pb.AppendBookmark('7'); $pb.AppendText('how are you??')
    }
    $sid = 'bm_' + [guid]::NewGuid().ToString('N')
    Register-ObjectEvent -InputObject $s -EventName BookmarkReached -SourceIdentifier $sid | Out-Null
    $t0 = Get-Date
    $s.SpeakAsync($pb) | Out-Null
    while ($s.State -ne 'Ready' -or ((Get-Date) - $t0).TotalMilliseconds -lt 200) {
        if (((Get-Date) - $t0).TotalSeconds -gt 30) { break }
        Start-Sleep -Milliseconds 50
    }
    Start-Sleep -Milliseconds 300
    $marks = @(Get-Event -SourceIdentifier $sid -ErrorAction SilentlyContinue).Count
    Unregister-Event -SourceIdentifier $sid
    Get-Event -SourceIdentifier $sid -ErrorAction SilentlyContinue | Remove-Event
    $s.SetOutputToNull(); $s.Dispose()
    $bytes = [System.IO.File]::ReadAllBytes($wav)
    $samples = [int](($bytes.Length - 44) / 2)
    $sum = 0.0; $n = 0
    for ($i = 44; $i -lt $bytes.Length - 1; $i += 8) { $x = [BitConverter]::ToInt16($bytes, $i); $sum += $x * $x; $n++ }
    $rms = if ($n) { [Math]::Sqrt($sum / $n) } else { 0 }
    $secs = $samples / 22050.0
    $ok = ($secs -gt 0.5) -and ($rms -gt 300) -and ($marks -ge 1)
    if (-not $ok) { $bad++ }
    Write-Host ("{0,-4} {1,-7} {2,-32} {3:N2} s, rms {4:N0}, bookmarks {5}, {6:N1} s" -f `
        $(if ($ok) { 'ok' } else { 'FAIL' }), $bits, $name, $secs, $rms, $marks, ((Get-Date) - $t0).TotalSeconds)
}
if ($bad) { exit 1 } else { exit 0 }
