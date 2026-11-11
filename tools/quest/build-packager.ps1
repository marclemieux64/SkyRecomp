[CmdletBinding()]
param([string]$OutputDirectory = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'build\quest-packager-release' }
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$publish = Join-Path $repo 'build\quest-packager-publish'
$stage = Join-Path $output "stage-$PID"
$archive = Join-Path $output 'SkyRecomp-Quest-Packager-Windows-x64.zip'

New-Item -ItemType Directory -Force -Path $output, $publish, $stage | Out-Null
& dotnet publish (Join-Path $PSScriptRoot 'gui\SkyRecompQuestPackager.csproj') `
    -c Release -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:DebugType=None -p:DebugSymbols=false -o $publish
if ($LASTEXITCODE -ne 0) { throw 'Packager executable compilation failed.' }

# Populate the portable bundle only with tracked source files, so private game
# data and locally generated libraries can never enter the release archive.
& git -C $repo archive --format=tar HEAD -o (Join-Path $output 'source.tar')
if ($LASTEXITCODE -ne 0) { throw 'Could not create the source payload.' }
& tar -xf (Join-Path $output 'source.tar') -C $stage
if ($LASTEXITCODE -ne 0) { throw 'Could not extract the source payload.' }
Copy-Item -LiteralPath (Join-Path $publish 'SkyRecomp-Quest-Packager.exe') -Destination $stage -Force
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
Remove-Item -LiteralPath (Join-Path $output 'source.tar') -Force
Remove-Item -LiteralPath $stage -Recurse -Force
Write-Output $archive
