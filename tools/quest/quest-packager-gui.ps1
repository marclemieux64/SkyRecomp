[CmdletBinding()]
param(
    [switch]$SelfTest,
    [switch]$AutoBuild,
    [string]$InitialGameRoot = '',
    [string]$InitialAndroidSdk = '',
    [string]$InitialJavaHome = ''
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
[System.Windows.Forms.Application]::EnableVisualStyles()

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$backend = Join-Path $PSScriptRoot 'build-personal-apk.ps1'
$defaultSdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$defaultJava = if ($env:JAVA_HOME) { $env:JAVA_HOME } else { '' }

$form = New-Object System.Windows.Forms.Form
$form.Text = 'SkyRecomp Quest Packager'
$form.Size = New-Object System.Drawing.Size(820, 620)
$form.MinimumSize = New-Object System.Drawing.Size(700, 500)
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$title = New-Object System.Windows.Forms.Label
$title.Text = 'Build your personal SkyRecomp Quest APK'
$title.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 15)
$title.AutoSize = $true
$title.Location = New-Object System.Drawing.Point(20, 18)
$form.Controls.Add($title)

$notice = New-Object System.Windows.Forms.Label
$notice.Text = "Select the folder containing your legally extracted exefs and romfs folders.`r`nGame files stay on this PC and are never uploaded."
$notice.AutoSize = $true
$notice.Location = New-Object System.Drawing.Point(22, 55)
$form.Controls.Add($notice)

function Add-PathRow([string]$label, [int]$top, [string]$initial) {
    $caption = New-Object System.Windows.Forms.Label
    $caption.Text = $label
    $caption.AutoSize = $true
    $caption.Location = [System.Drawing.Point]::new(22, ($top + 5))
    $form.Controls.Add($caption)
    $box = New-Object System.Windows.Forms.TextBox
    $box.Text = $initial
    $box.Anchor = 'Top,Left,Right'
    $box.Location = New-Object System.Drawing.Point(150, $top)
    $box.Size = New-Object System.Drawing.Size(560, 25)
    $form.Controls.Add($box)
    $browse = New-Object System.Windows.Forms.Button
    $browse.Text = 'Browse…'
    $browse.Anchor = 'Top,Right'
    $browse.Location = [System.Drawing.Point]::new(715, ($top - 1))
    $browse.Size = New-Object System.Drawing.Size(75, 27)
    $browse.Add_Click({
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        if ($box.Text -and (Test-Path -LiteralPath $box.Text)) { $dialog.SelectedPath = $box.Text }
        if ($dialog.ShowDialog() -eq 'OK') { $box.Text = $dialog.SelectedPath }
    }.GetNewClosure())
    $form.Controls.Add($browse)
    return $box
}

$gameBox = Add-PathRow 'Extracted game:' 105 $InitialGameRoot
$sdkBox = Add-PathRow 'Android SDK:' 142 $(if ($InitialAndroidSdk) { $InitialAndroidSdk } else { $defaultSdk })
$javaBox = Add-PathRow 'Java/JDK:' 179 $(if ($InitialJavaHome) { $InitialJavaHome } else { $defaultJava })

$install = New-Object System.Windows.Forms.CheckBox
$install.Text = 'Install to a connected Quest after building'
$install.AutoSize = $true
$install.Location = New-Object System.Drawing.Point(150, 217)
$form.Controls.Add($install)

$build = New-Object System.Windows.Forms.Button
$build.Text = 'Build personal APK'
$build.Location = New-Object System.Drawing.Point(150, 252)
$build.Size = New-Object System.Drawing.Size(170, 36)
$form.Controls.Add($build)

$openOutput = New-Object System.Windows.Forms.Button
$openOutput.Text = 'Open output folder'
$openOutput.Location = New-Object System.Drawing.Point(330, 252)
$openOutput.Size = New-Object System.Drawing.Size(150, 36)
$openOutput.Add_Click({
    $path = Join-Path $repo 'android\app\build\outputs\apk\personal\release'
    if (Test-Path -LiteralPath $path) { Start-Process explorer.exe -ArgumentList "`"$path`"" }
})
$form.Controls.Add($openOutput)

$status = New-Object System.Windows.Forms.Label
$status.Text = 'Ready'
$status.AutoSize = $true
$status.Location = New-Object System.Drawing.Point(500, 262)
$form.Controls.Add($status)

$log = New-Object System.Windows.Forms.TextBox
$log.Multiline = $true
$log.ReadOnly = $true
$log.ScrollBars = 'Both'
$log.WordWrap = $false
$log.Anchor = 'Top,Bottom,Left,Right'
$log.Location = New-Object System.Drawing.Point(22, 305)
$log.Size = New-Object System.Drawing.Size(768, 265)
$log.Font = New-Object System.Drawing.Font('Consolas', 8.5)
$form.Controls.Add($log)

$script:process = $null
$build.Add_Click({
    foreach ($pair in @(@('game folder', $gameBox.Text), @('Android SDK', $sdkBox.Text), @('Java/JDK', $javaBox.Text))) {
        if (-not $pair[1] -or -not (Test-Path -LiteralPath $pair[1])) {
            [System.Windows.Forms.MessageBox]::Show("The $($pair[0]) path is missing or invalid.", 'SkyRecomp Quest Packager', 'OK', 'Error') | Out-Null
            return
        }
    }
    $quote = { param($value) '"' + ($value -replace '"', '\"') + '"' }
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File $(& $quote $backend) -GameRoot $(& $quote $gameBox.Text) -AndroidSdk $(& $quote $sdkBox.Text) -JavaHome $(& $quote $javaBox.Text)"
    if ($install.Checked) { $arguments += ' -Install' }
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = 'powershell.exe'
    $info.Arguments = $arguments
    $info.WorkingDirectory = $repo
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $script:process = New-Object System.Diagnostics.Process
    $script:process.StartInfo = $info
    $script:process.EnableRaisingEvents = $true
    $script:process.SynchronizingObject = $form
    $script:process.add_OutputDataReceived({ param($sender, $eventArgs) if ($eventArgs.Data) { $log.AppendText($eventArgs.Data + "`r`n") } })
    $script:process.add_ErrorDataReceived({ param($sender, $eventArgs) if ($eventArgs.Data) { $log.AppendText($eventArgs.Data + "`r`n") } })
    $script:process.add_Exited({
        $code = $script:process.ExitCode
        $build.Enabled = $true
        $status.Text = if ($code -eq 0) { 'Build completed' } else { "Build failed (exit $code)" }
    })
    $log.Clear()
    $build.Enabled = $false
    $status.Text = 'Building…'
    $script:process.Start() | Out-Null
    $script:process.BeginOutputReadLine()
    $script:process.BeginErrorReadLine()
})

if ($AutoBuild) {
    $form.Add_Shown({ $build.PerformClick() })
    $form.Add_FormClosing({
        if ($script:process -and -not $script:process.HasExited) { $_.Cancel = $true }
    })
    $timer = New-Object System.Windows.Forms.Timer
    $timer.Interval = 250
    $timer.Add_Tick({
        if ($script:process -and $script:process.HasExited) {
            $script:autoExitCode = $script:process.ExitCode
            $timer.Stop()
            $form.Close()
        }
    })
    $timer.Start()
}

if ($SelfTest) {
    if ($form.Text -ne 'SkyRecomp Quest Packager' -or -not $build.Enabled -or
        -not $gameBox -or -not $sdkBox -or -not $javaBox -or -not $log) {
        throw 'GUI control construction self-test failed.'
    }
    Write-Output 'Quest Packager GUI self-test passed.'
    $form.Dispose()
    exit 0
}
[void]$form.ShowDialog()
if ($AutoBuild) {
    Write-Output $log.Text
    exit $script:autoExitCode
}
