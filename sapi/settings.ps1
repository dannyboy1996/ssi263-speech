# SSI-263 SAPI settings -- the voices, their registration, and the settings SAPI's own requests cannot carry.
#
# A trimmed sibling of outspoken-nvda's sapi/settings.ps1 (itself Panthera's), with the same layout: the voice
# list comes from the pipe server itself (the one authority on what is installed), registration goes through
# register.ps1 elevated, and the engine settings sit below.  Rate, pitch and volume stay SAPI's own.  The
# settings are this person's (HKCU "Software\SSI-263 SAPI"); the engine DLL reads them before every utterance and
# replaces its server when they change, so a change reaches the next thing spoken, in every SAPI program at once.
#
# PowerShell 2.0's dialect, like register.ps1: stock Windows 7 has no newer engine.
# -Check: build the dialog and fill it, print what it shows, and exit without showing it (the tests).
param([switch]$Check)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$stage = Split-Path -Parent $MyInvocation.MyCommand.Path
$registerScript = Join-Path $stage 'register.ps1'
$prefKey = 'HKCU:\Software\SSI-263 SAPI'
$tokenRoots = @('HKLM:\SOFTWARE\Microsoft\Speech\Voices\Tokens', 'HKLM:\SOFTWARE\Wow6432Node\Microsoft\Speech\Voices\Tokens')

function Load-Setting([string]$name, [int]$default) {
    try { [int](Get-ItemProperty -Path $prefKey -Name $name -ErrorAction Stop).$name }
    catch { $default }
}
function Save-Setting([string]$name, [int]$value) {
    New-Item -Path $prefKey -Force | Out-Null
    New-ItemProperty -Path $prefKey -Name $name -Value $value -PropertyType DWord -Force | Out-Null
}

# The voices the server lists: "id<TAB>name<TAB>language", UTF-8.
function Get-ServerVoices {
    $voices = @()
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = Join-Path $stage 'python\python.exe'
        $psi.Arguments = "-I `"$(Join-Path $stage 'ssi_serve.py')`" --list"
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.CreateNoWindow = $true
        $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
        $p = [System.Diagnostics.Process]::Start($psi)
        $out = $p.StandardOutput.ReadToEnd()
        $p.WaitForExit()
        foreach ($line in ($out -split "`r?`n")) {
            if ($line -match "`t") { $voices += ,($line.Split("`t")) }
        }
    } catch {}
    ,$voices
}
function Test-Registered([string]$id) {
    $key = 'SSI263_' + ($id -replace '[^A-Za-z0-9]', '_')
    foreach ($root in $tokenRoots) {
        if (Test-Path (Join-Path $root $key)) { return $true }
    }
    $false
}

# A cancelled elevation prompt is not a yes: -1, and the caller says nothing (outspoken's lesson).
function Invoke-Elevated([string]$switches) {
    $arguments = '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{0}" {1}' -f $registerScript, $switches
    $process = $null
    try {
        $process = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $arguments -ErrorAction Stop
    } catch { return -1 }
    if (-not $process) { return -1 }
    $process.ExitCode
}

$form = New-Object Windows.Forms.Form
$form.Text = 'SSI-263 SAPI settings'; $form.Size = New-Object Drawing.Size(640, 470)
$form.StartPosition = 'CenterScreen'
$label = New-Object Windows.Forms.Label
$label.Text = '&Voices:'; $label.AutoSize = $true; $label.Location = New-Object Drawing.Point(12, 14)
$list = New-Object Windows.Forms.ListBox
$list.Name = 'voiceList'; $list.AccessibleName = 'Voices'
$list.AccessibleDescription = 'The emulated SSI-263 voices this installation carries, and whether each is registered with SAPI'
$list.Location = New-Object Drawing.Point(12, 38); $list.Size = New-Object Drawing.Size(600, 120)
$status = New-Object Windows.Forms.Label
$status.Name = 'status'; $status.AccessibleName = 'Status'
$status.Location = New-Object Drawing.Point(12, 166); $status.Size = New-Object Drawing.Size(600, 40)

function Refresh-Voices {
    $list.Items.Clear()
    $voices = Get-ServerVoices
    $registered = 0
    foreach ($v in $voices) {
        $state = 'not registered'
        if (Test-Registered $v[0]) { $state = 'registered'; $registered++ }
        [void]$list.Items.Add(('{0} - {1}' -f $v[1], $state))
    }
    if ($list.Items.Count) { $list.SelectedIndex = 0 }
    if (-not $voices.Count) {
        $status.Text = 'The voices could not be listed: the installation may be damaged. Reinstall SSI-263 SAPI.'
    } elseif ($registered -eq $voices.Count) {
        $status.Text = 'All {0} voices are registered with SAPI, for 32-bit and 64-bit programs.' -f $voices.Count
    } elseif ($registered -eq 0) {
        $status.Text = 'No voice is registered with SAPI. Use Register to make them available.'
    } else {
        $status.Text = '{0} of {1} voices are registered. Use Register to add the rest.' -f $registered, $voices.Count
    }
}

$register = New-Object Windows.Forms.Button; $register.Text = '&Register'; $register.AutoSize = $true
$register.Location = New-Object Drawing.Point(12, 214)
$unregister = New-Object Windows.Forms.Button; $unregister.Text = '&Unregister'; $unregister.AutoSize = $true
$unregister.Location = New-Object Drawing.Point(110, 214)
$close = New-Object Windows.Forms.Button; $close.Text = '&Close'; $close.AutoSize = $true
$close.Location = New-Object Drawing.Point(208, 214)

# The Braille Lite's two settings, as in the NVDA add-on.
$group = New-Object Windows.Forms.GroupBox
$group.Text = 'Braille Lite'; $group.Location = New-Object Drawing.Point(12, 256); $group.Size = New-Object Drawing.Size(600, 100)
$inflection = New-Object Windows.Forms.CheckBox
$inflection.Text = 'Voice &inflection'
$inflection.AccessibleName = 'Voice inflection'
$inflection.AccessibleDescription = 'The unit''s own status-menu setting. Off, questions and sentence ends stay flat.'
$inflection.Location = New-Object Drawing.Point(12, 24); $inflection.AutoSize = $true
$whineLabel = New-Object Windows.Forms.Label
$whineLabel.Text = 'Unit &hiss and whine:'; $whineLabel.AutoSize = $true
$whineLabel.Location = New-Object Drawing.Point(12, 60)
$whine = New-Object Windows.Forms.ComboBox
$whine.DropDownStyle = 'DropDownList'; $whine.AccessibleName = 'Unit hiss and whine'
$whine.AccessibleDescription = 'The faint sound a real Braille Lite makes under its speech, generated from the chip''s clock. Off by default.'
$whine.Location = New-Object Drawing.Point(150, 57); $whine.Size = New-Object Drawing.Size(300, 24)
foreach ($item in @('Off', 'Hiss (even volumes, as the factory setting)', 'Whine (odd volumes)')) { [void]$whine.Items.Add($item) }
$diagnostics = New-Object Windows.Forms.CheckBox
$diagnostics.Text = 'Write a &diagnostic log'
$diagnostics.AccessibleName = 'Write a diagnostic log'
$diagnostics.AccessibleDescription = 'Off by default. Records what the engine did, not what was spoken, to a file in your temp folder. Turn it on only if a bug report asks for it.'
$diagnostics.Location = New-Object Drawing.Point(12, 370); $diagnostics.AutoSize = $true

$inflection.Checked = [bool](Load-Setting 'Inflection' 1)
$whine.SelectedIndex = [Math]::Max(0, [Math]::Min(2, (Load-Setting 'Whine' 0)))
$diagnostics.Checked = [bool](Load-Setting 'Diagnostics' 0)
$inflection.Add_CheckedChanged({ Save-Setting 'Inflection' ([int]$inflection.Checked) })
$whine.Add_SelectedIndexChanged({ if ($whine.SelectedIndex -ge 0) { Save-Setting 'Whine' $whine.SelectedIndex } })
$diagnostics.Add_CheckedChanged({ Save-Setting 'Diagnostics' ([int]$diagnostics.Checked) })

$register.Add_Click({
    $code = Invoke-Elevated '-Register'
    if ($code -gt 0) { [Windows.Forms.MessageBox]::Show($form, 'Registration failed.', 'SSI-263 SAPI', 'OK', 'Error') | Out-Null }
    elseif ($code -eq 0) { [Windows.Forms.MessageBox]::Show($form, 'The SSI-263 voices were registered for 32-bit and 64-bit SAPI.', 'SSI-263 SAPI') | Out-Null }
    Refresh-Voices
})
$unregister.Add_Click({
    $code = Invoke-Elevated '-Unregister'
    if ($code -gt 0) { [Windows.Forms.MessageBox]::Show($form, 'Unregistration failed.', 'SSI-263 SAPI', 'OK', 'Error') | Out-Null }
    elseif ($code -eq 0) { [Windows.Forms.MessageBox]::Show($form, 'The SSI-263 voices were unregistered.', 'SSI-263 SAPI') | Out-Null }
    Refresh-Voices
})
$close.Add_Click({ $form.Close() })
$form.CancelButton = $close
$group.Controls.AddRange(@($inflection, $whineLabel, $whine))
$form.Controls.AddRange(@($label, $list, $status, $register, $unregister, $close, $group, $diagnostics))
Refresh-Voices
if ($Check) {
    foreach ($item in $list.Items) { Write-Output ('voice: ' + $item) }
    Write-Output ('status: ' + $status.Text)
    Write-Output ('inflection: {0}; whine: {1}; diagnostics: {2}' -f $inflection.Checked, $whine.SelectedItem, $diagnostics.Checked)
    exit 0
}
$form.Add_Shown({ $list.Focus() })
[void]$form.ShowDialog()
