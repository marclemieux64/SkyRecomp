[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,
    [string]$AndroidSdk = $env:ANDROID_HOME,
    [string]$JavaHome = $env:JAVA_HOME,
    [switch]$Install
)

$ErrorActionPreference = 'Stop'
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

$generated = Join-Path $repo 'build\quest-personal\generated'
$nativeBuild = Join-Path $repo 'build\quest-personal\native'
$privateJni = Join-Path $repo 'android\app\src\personal\jniLibs\arm64-v8a'
New-Item -ItemType Directory -Force -Path $generated, $nativeBuild, $privateJni | Out-Null

$hostPython = (Get-Command python -ErrorAction Stop).Source
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
foreach ($module in $modules) {
    $out = Join-Path $generated $module[0]
    $args = @((Join-Path $repo 'tools\gen.py'), $module[0], $module[1], $module[2], $out, '--simd', 'asm')
    if ($module[0] -eq 'sdk') { $args += @('--hle', (Join-Path $repo 'runtime\hle\hle_symbols.txt')) }
    & $python @args
    if ($LASTEXITCODE -ne 0) { throw "Game-code generation failed for $($module[0])." }
}

& $cmake -S (Join-Path $repo 'android\game') -B $nativeBuild -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $ndk 'build\cmake\android.toolchain.cmake')" `
    '-DANDROID_ABI=arm64-v8a' '-DANDROID_PLATFORM=android-29' `
    "-DSKYRECOMP_ROOT=$repo" "-DSKYRECOMP_GENERATED_DIR=$generated"
if ($LASTEXITCODE -ne 0) { throw 'Android game-library configuration failed.' }
& $cmake --build $nativeBuild --target skygame --parallel
if ($LASTEXITCODE -ne 0) { throw 'Android game-library build failed.' }
Copy-Item -LiteralPath (Join-Path $nativeBuild 'libskygame.so') -Destination $privateJni -Force

$env:JAVA_HOME = $JavaHome
$env:ANDROID_HOME = $AndroidSdk
$env:ANDROID_SDK_ROOT = $AndroidSdk
& (Join-Path $repo 'android\gradlew.bat') -p (Join-Path $repo 'android') assemblePersonalRelease
if ($LASTEXITCODE -ne 0) { throw 'Personal Quest APK build failed.' }
$apk = Get-ChildItem (Join-Path $repo 'android\app\build\outputs\apk\personal\release\*.apk') |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $apk) { throw 'Gradle completed but produced no personal APK.' }
Write-Host "Personal APK: $($apk.FullName)"

if ($Install) {
    if (-not (Test-Path -LiteralPath $adb)) { throw "adb not found: $adb" }
    & $adb install -r $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw 'APK installation failed.' }
    & $adb shell mkdir -p /sdcard/Android/data/io.github.skyrecomp.quest/files/extracted
    & $adb push $romfs /sdcard/Android/data/io.github.skyrecomp.quest/files/extracted/
    & $adb push $exefs /sdcard/Android/data/io.github.skyrecomp.quest/files/extracted/
}
