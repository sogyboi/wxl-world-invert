[CmdletBinding()]
param(
    [string]$BuildDir = "$PSScriptRoot\..\client\.build\release"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedBuild = (Resolve-Path -LiteralPath $BuildDir).Path
$dll = Join-Path $resolvedBuild 'Release\wxl-world-invert.dll'
$manifest = Join-Path $root 'client\wxl.json'
$readme = Join-Path $root 'README.md'
$license = Join-Path $root 'LICENSE'
$addon = Join-Path $root 'addons\WorldMirrorControls'
foreach ($path in @($dll, $manifest, $readme, $license)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required package input: $path"
    }
}
if (-not (Test-Path -LiteralPath $addon -PathType Container)) {
    throw "Missing required addon directory: $addon"
}

$manifestObject = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
$packageRoot = Join-Path $root ("artifacts\wxl-world-invert-v{0}" -f $manifestObject.extension.version)
if (Test-Path -LiteralPath $packageRoot) {
    throw "Refusing to overwrite existing package: $packageRoot"
}

$extensionRoot = Join-Path $packageRoot 'Extensions\wxl-world-invert'
$addonRoot = Join-Path $packageRoot 'Interface\AddOns\WorldMirrorControls'
New-Item -ItemType Directory -Path $extensionRoot -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Path $addonRoot -ErrorAction Stop | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $extensionRoot 'wxl-world-invert.dll') -ErrorAction Stop
Copy-Item -LiteralPath $manifest -Destination (Join-Path $extensionRoot 'wxl.json') -ErrorAction Stop
Copy-Item -LiteralPath $readme -Destination (Join-Path $extensionRoot 'README.md') -ErrorAction Stop
Copy-Item -LiteralPath $license -Destination (Join-Path $extensionRoot 'LICENSE') -ErrorAction Stop
Copy-Item -Path (Join-Path $addon '*') -Destination $addonRoot -Recurse -Force -ErrorAction Stop

Get-ChildItem -LiteralPath $packageRoot -File -Recurse |
    Sort-Object Name |
    Get-FileHash -Algorithm SHA256 |
    Select-Object Algorithm, Hash, Path |
    Format-Table -AutoSize
Write-Host "Packaged: $packageRoot"
