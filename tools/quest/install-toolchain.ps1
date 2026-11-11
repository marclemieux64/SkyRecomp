[CmdletBinding()]
param(
    [string]$Destination = '',
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Destination) { $Destination = Join-Path $repo 'toolchain' }
$toolchain = [System.IO.Path]::GetFullPath($Destination)
$jdkRoot = Join-Path $toolchain 'jdk'
$sdkRoot = Join-Path $toolchain 'android-sdk'
$downloads = Join-Path $toolchain 'downloads'
$androidToolsUrl = 'https://dl.google.com/android/repository/commandlinetools-win-15859902_latest.zip'
$androidToolsSha256 = '90ae805d20434428bffcb699c290860f19bb5f66a67e6b330067e3de801fb04a'
$jdkUrl = 'https://aka.ms/download-jdk/microsoft-jdk-21-windows-x64.zip'

function Download-File([string]$Uri, [string]$Path) {
    $curl = (Get-Command curl.exe -ErrorAction Stop).Source
    & $curl --location --fail --retry 3 --progress-bar --output $Path $Uri
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Uri" }
}

if ($SelfTest) {
    if ($androidToolsUrl -notmatch '^https://dl\.google\.com/' -or $androidToolsSha256.Length -ne 64 -or
        $jdkUrl -notmatch '^https://aka\.ms/') { throw 'Toolchain metadata self-test failed.' }
    Write-Output 'Quest toolchain installer self-test passed.'
    exit 0
}

New-Item -ItemType Directory -Force -Path $toolchain, $downloads, $jdkRoot, $sdkRoot | Out-Null
$jdkZip = Join-Path $downloads 'microsoft-jdk-21-windows-x64.zip'
$androidZip = Join-Path $downloads 'android-command-line-tools.zip'

$java = Get-ChildItem $jdkRoot -Recurse -Filter java.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -Match '\\bin\\java\.exe$' | Select-Object -First 1
if (-not $java) {
    Write-Output '== Tools 1/3: Downloading Java (about 200 MB)'
    Download-File $jdkUrl $jdkZip
    Expand-Archive -LiteralPath $jdkZip -DestinationPath $jdkRoot -Force
    $java = Get-ChildItem $jdkRoot -Recurse -Filter java.exe |
        Where-Object FullName -Match '\\bin\\java\.exe$' | Select-Object -First 1
    if (-not $java) { throw 'The JDK archive did not contain bin/java.exe.' }
}
$javaHome = Split-Path (Split-Path $java.FullName)
$env:JAVA_HOME = $javaHome

$sdkManager = Join-Path $sdkRoot 'cmdline-tools\latest\bin\sdkmanager.bat'
if (-not (Test-Path -LiteralPath $sdkManager)) {
    Write-Output '== Tools 2/3: Downloading the Android command-line tools'
    Download-File $androidToolsUrl $androidZip
    $actualHash = (Get-FileHash -LiteralPath $androidZip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $androidToolsSha256) { throw "Android tools checksum mismatch: $actualHash" }
    $unpack = Join-Path $toolchain 'android-tools-unpack'
    New-Item -ItemType Directory -Force -Path $unpack | Out-Null
    Expand-Archive -LiteralPath $androidZip -DestinationPath $unpack -Force
    $latest = Join-Path $sdkRoot 'cmdline-tools\latest'
    New-Item -ItemType Directory -Force -Path $latest | Out-Null
    Copy-Item -Path (Join-Path $unpack 'cmdline-tools\*') -Destination $latest -Recurse -Force
    if (-not (Test-Path -LiteralPath $sdkManager)) { throw 'Android sdkmanager was not installed correctly.' }
}

Write-Output 'Accepting Android SDK licenses...'
1..100 | ForEach-Object { 'y' } | & $sdkManager "--sdk_root=$sdkRoot" --licenses | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Android SDK license acceptance failed.' }
Write-Output '== Tools 3/3: Downloading the Android compiler and SDK (about 2 GB, several minutes)'
& $sdkManager "--sdk_root=$sdkRoot" `
    'platforms;android-36' 'build-tools;35.0.0' 'platform-tools' `
    'ndk;27.1.12297006' 'cmake;3.31.6'
if ($LASTEXITCODE -ne 0) { throw 'Android SDK package installation failed.' }

$config = [ordered]@{ androidSdk = $sdkRoot; javaHome = $javaHome }
$config | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $toolchain 'toolchain.json') -Encoding UTF8
Write-Output "Android SDK: $sdkRoot"
Write-Output "Java home: $javaHome"
