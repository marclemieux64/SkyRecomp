[CmdletBinding()]
param([string]$Destination = '')

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$expected = [System.IO.Path]::GetFullPath((Join-Path $repo 'toolchain'))
if (-not $Destination) { $Destination = $expected }
$target = [System.IO.Path]::GetFullPath($Destination)

# Never allow this cleanup helper to target a system-wide SDK or JDK.
if ($target -ne $expected) { throw "Refusing to remove a non-private toolchain path: $target" }
if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
    Write-Output "Removed private toolchain: $target"
} else {
    Write-Output 'The private toolchain is already absent.'
}
