[CmdletBinding()]
param([string]$OutputDirectory = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'build\quest-packager-release' }
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$publish = Join-Path $repo 'build\quest-packager-publish'
$stage = Join-Path $output "payload-$PID"
$payload = Join-Path $output 'payload.zip'
$executable = Join-Path $output 'Skyrecomp-VR APK Builder.exe'

New-Item -ItemType Directory -Force -Path $output, $publish, $stage | Out-Null

# The payload: only the files tools/quest/payload-files.txt lists, and of those only what Git tracks or would
# track - so local edits are tested before a push, while ignored game data can never be included.
$wanted = Get-Content (Join-Path $PSScriptRoot 'payload-files.txt') |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') }
$tracked = & git -C $repo ls-files --cached --others --exclude-standard
if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate the source payload.' }
$copied = 0
foreach ($relative in $tracked) {
    $listed = $false
    foreach ($entry in $wanted) {
        if (($entry.EndsWith('/') -and $relative.StartsWith($entry)) -or $relative -ceq $entry) { $listed = $true; break }
    }
    if (-not $listed) { continue }
    $source = Join-Path $repo $relative
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $copied++
}
foreach ($entry in $wanted) {
    if (-not $entry.EndsWith('/') -and -not (Test-Path -LiteralPath (Join-Path $stage $entry))) {
        throw "A file listed in payload-files.txt is missing: $entry"
    }
}
Write-Host "Payload: $copied source files"
if (Test-Path -LiteralPath $payload) { Remove-Item -LiteralPath $payload -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $payload -CompressionLevel Optimal

& dotnet publish (Join-Path $PSScriptRoot 'gui\SkyRecompQuestPackager.csproj') `
    -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:DebugType=None -p:DebugSymbols=false `
    "-p:PayloadPath=$payload" -o $publish
if ($LASTEXITCODE -ne 0) { throw 'Packager executable compilation failed.' }
Copy-Item -LiteralPath (Join-Path $publish 'Skyrecomp-VR APK Builder.exe') -Destination $executable -Force
Remove-Item -LiteralPath $stage -Recurse -Force
Remove-Item -LiteralPath $payload -Force
Write-Output $executable
