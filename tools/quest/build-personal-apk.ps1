[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$JavaHome = $env:JAVA_HOME,
    [switch]$Install,      # install on a connected headset after the build (skipped with a note when none is connected)
    [switch]$InstallOnly   # skip the build: install the APK that is already built
)

$ErrorActionPreference = 'Stop'
$env:PYTHONUNBUFFERED = '1'  # progress lines arrive as they are printed, not in bursts
if ($InstallOnly) { $Install = $true }
$totalSteps = if ($Install) { 6 } else { 5 }
function Step([int]$Number, [string]$Text) { Write-Host "== Step $Number/${totalSteps}: $Text" }

Step 1 'Checking the game files and build tools'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$game = (Resolve-Path $GameRoot).Path
$exefs = Join-Path $game 'exefs'
$romfs = Join-Path $game 'romfs'
$required = 'main', 'sdk', 'subsdk0', 'subsdk1', 'main.npdm'
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $exefs $name))) {
        throw "Missing extracted game file: exefs/$name"
    }
}
if (-not (Test-Path -LiteralPath $romfs -PathType Container)) {
    throw 'Missing extracted game directory: romfs'
}
if (-not $AndroidSdk) { throw 'Set ANDROID_HOME or pass -AndroidSdk.' }
if (-not $JavaHome) { throw 'Set JAVA_HOME or pass -JavaHome.' }

$ndkVersion = '27.1.12297006'
$cmakeVersion = '3.31.6'
$ndk = Join-Path $AndroidSdk "ndk\$ndkVersion"
$cmake = Join-Path $AndroidSdk "cmake\$cmakeVersion\bin\cmake.exe"
$ninja = Join-Path $AndroidSdk "cmake\$cmakeVersion\bin\ninja.exe"
$adb = Join-Path $AndroidSdk 'platform-tools\adb.exe'
foreach ($tool in $cmake, $ninja) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Android build tool not found: $tool" }
}

if (-not $InstallOnly) {
    $generated = Join-Path $repo 'build\quest-personal\generated'
    $nativeBuild = Join-Path $repo 'build\quest-personal\native'
    $privateJni = Join-Path $repo 'android\app\src\personal\jniLibs\arm64-v8a'
    New-Item -ItemType Directory -Force -Path $generated, $nativeBuild, $privateJni | Out-Null

    $hostCommand = Get-Command python -ErrorAction SilentlyContinue
    if (-not $hostCommand) { throw 'Python 3 was not found. Install it from https://www.python.org/downloads/ and try again.' }
    $hostPython = $hostCommand.Source
    $pythonEnv = Join-Path $repo 'build\quest-personal\python-env'
    $python = Join-Path $pythonEnv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $python)) {
        Write-Host 'Preparing the private code-generation environment...'
        & $hostPython -m venv $pythonEnv
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the private Python environment.' }
    }
    $savedErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $null = & $python -c 'import unicorn, capstone, lz4.block' 2>$null
    $unicornImportExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorAction
    if ($unicornImportExitCode -ne 0) {
        Write-Host 'Installing the pinned code-generation dependencies...'
        & $python -m pip install --disable-pip-version-check 'unicorn==2.1.4' 'capstone==5.0.9' 'lz4==4.4.5'
        if ($LASTEXITCODE -ne 0) { throw 'Could not install the private Python code-generation environment.' }
    }
    $modules = @(
        @('main',    (Join-Path $exefs 'main'),    '0x7100004000'),
        @('sdk',     (Join-Path $exefs 'sdk'),     '0x712E74B000'),
        @('subsdk0', (Join-Path $exefs 'subsdk0'), '0x712D037000'),
        @('subsdk1', (Join-Path $exefs 'subsdk1'), '0x712D9FF000')
    )
    Step 2 'Translating the game code to C (4 parts, several minutes)'
    foreach ($module in $modules) {
        Write-Host "Analysing $($module[0])... (the first progress line can take a minute)"
        $out = Join-Path $generated $module[0]
        $args = @((Join-Path $repo 'tools\gen.py'), $module[0], $module[1], $module[2], $out, '--simd', 'asm')
        if ($module[0] -eq 'sdk') { $args += @('--hle', (Join-Path $repo 'runtime\hle\hle_symbols.txt')) }
        & $python @args
        if ($LASTEXITCODE -ne 0) { throw "Game-code generation failed for $($module[0])." }
    }

    Step 3 'Compiling the game code for the headset (about 2000 files, the longest step)'
    & $cmake -S (Join-Path $repo 'android\game') -B $nativeBuild -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $ndk 'build\cmake\android.toolchain.cmake')" `
        '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-29' `
        "-DSKYRECOMP_ROOT=$repo" "-DSKYRECOMP_GENERATED_DIR=$generated"
    if ($LASTEXITCODE -ne 0) { throw 'Android game-library configuration failed.' }
    & $cmake --build $nativeBuild --target skygame --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Android game-library build failed.' }
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'libskygame.so') -Destination $privateJni -Force

    Step 4 'Building the game engine (the first build downloads its libraries and takes a few minutes)'
    $env:JAVA_HOME = $JavaHome
    $env:ANDROID_HOME = $AndroidSdk
    $env:ANDROID_SDK_ROOT = $AndroidSdk
    & (Join-Path $repo 'android\gradlew.bat') -p (Join-Path $repo 'android') --console=plain assemblePersonalRelease
    if ($LASTEXITCODE -ne 0) { throw 'Personal Quest APK build failed.' }
}
$apk = Get-ChildItem (Join-Path $repo 'android\app\build\outputs\apk\personal\release\*.apk') |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $apk) { throw 'No personal APK has been built yet. Build it first.' }
Step 5 'The APK is ready'
Write-Host "Personal APK: $($apk.FullName)"

if ($Install) {
    $package = 'io.github.skyrecomp.quest'
    $remote = "/sdcard/Android/data/$package/files/extracted"
    if (-not (Test-Path -LiteralPath $adb)) { throw "adb not found: $adb" }
    $devices = @(& $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\S' })
    $ready = @($devices | Where-Object { $_ -match '\tdevice\s*$' })
    if ($ready.Count -eq 0) {
        if ($devices | Where-Object { $_ -match 'unauthorized' }) {
            Write-Host 'HEADSET: put the headset on and accept "Allow USB debugging", then click Install on headset.'
        } else {
            Write-Host 'HEADSET: none connected. Plug the Quest in by USB (developer mode on), then click Install on headset.'
        }
        exit 0
    }
    if ($ready.Count -gt 1) { throw 'More than one Android device is connected. Unplug the others and try again.' }

    Step 6 'Installing on the headset'
    Write-Host 'Installing the app...'
    & $adb install -r $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw 'APK installation failed.' }

    # The game data (about 7 GB) only needs copying once: a marker on the headset records what was copied.
    $files = Get-ChildItem -LiteralPath $romfs, $exefs -Recurse -File
    $bytes = ($files | Measure-Object -Property Length -Sum).Sum
    $fingerprint = "$($files.Count) files, $bytes bytes"
    $present = (& $adb shell "cat '$remote/.complete' 2>/dev/null" | Out-String).Trim()
    if ($present -eq $fingerprint) {
        Write-Host 'The game files are already on the headset.'
    } else {
        Write-Host ("Copying the game files to the headset ({0:N1} GB, this takes several minutes)..." -f ($bytes / 1GB))
        & $adb shell "mkdir -p '$remote' && rm -f '$remote/.complete'"
        & $adb push $romfs "$remote/"
        if ($LASTEXITCODE -ne 0) { throw 'Copying the game files failed. Check the headset has enough free space.' }
        & $adb push $exefs "$remote/"
        if ($LASTEXITCODE -ne 0) { throw 'Copying the game files failed. Check the headset has enough free space.' }
        & $adb shell "echo '$fingerprint' > '$remote/.complete'"
    }
    Write-Host 'HEADSET: installed. Start "SkyRecomp Quest Personal Build" from Unknown Sources in the headset library.'
}
