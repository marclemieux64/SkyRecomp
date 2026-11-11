[CmdletBinding()]
param([string]$Destination = '')

$ErrorActionPreference = 'Stop'
$expected = [System.IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Skyrecomp-VR APK Builder\toolchain'))
if (-not $Destination) { $Destination = $expected }
$target = [System.IO.Path]::GetFullPath($Destination)

# Never allow this cleanup helper to target a system-wide SDK or JDK.
if ($target -ne $expected) { throw "Refusing to remove a non-private toolchain path: $target" }
if (Test-Path -LiteralPath $target) {
    $prefix = $target.TrimEnd('\') + '\'
    Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
        Write-Output "Stopping private tool process: $($_.Name) ($($_.ProcessId))"
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop
            break
        } catch {
            if ($attempt -eq 10) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
    Write-Output "Removed private toolchain: $target"
} else {
    Write-Output 'The private toolchain is already absent.'
}
