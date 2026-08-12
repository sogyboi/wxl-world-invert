[CmdletBinding()]
param(
    [string]$WxlSdkRoot = $env:WXL_SDK_ROOT,
    [string]$BuildDir = "$PSScriptRoot\.build\release",
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$expectedSdkCommit = '439d5a90235a8f38ea3ca3d91541f0cf0a626fe1'
if ([string]::IsNullOrWhiteSpace($WxlSdkRoot)) {
    throw 'Set WXL_SDK_ROOT or pass -WxlSdkRoot <path> to the pinned WXL ABI-1.1 SDK checkout.'
}
$resolvedSdk = (Resolve-Path -LiteralPath $WxlSdkRoot).Path
$resolvedBuild = [System.IO.Path]::GetFullPath($BuildDir)

if ($Clean -and (Test-Path -LiteralPath $resolvedBuild)) {
    Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
}

$actualSdkCommit = (git -C $resolvedSdk rev-parse HEAD).Trim()
if ($actualSdkCommit -ne $expectedSdkCommit) {
    throw "Refusing unpinned WXL SDK. Expected $expectedSdkCommit, got $actualSdkCommit."
}

cmake -S $PSScriptRoot -B $resolvedBuild -G 'Visual Studio 16 2019' -A Win32 "-DWXL_SDK_ROOT=$resolvedSdk"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

cmake --build $resolvedBuild --config Release --target wxl-world-invert wxl-world-invert-static-tests wxl-world-invert-abi-probe
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }

& "$resolvedBuild\Release\wxl-world-invert-static-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Static tests failed.' }

& "$resolvedBuild\Release\wxl-world-invert-abi-probe.exe" "$resolvedBuild\Release\wxl-world-invert.dll"
if ($LASTEXITCODE -ne 0) { throw 'ABI probe failed.' }

$manifest = Get-Content -LiteralPath "$PSScriptRoot\wxl.json" -Raw | ConvertFrom-Json
if ($manifest.manifest -ne 1 -or $manifest.extension.id -ne 'wxl-world-invert' -or
    $manifest.extension.abi -ne '1.1' -or $manifest.extension.entry -ne 'wxl-world-invert.dll') {
    throw 'wxl.json failed manifest readiness validation.'
}

Write-Host "Ready: $resolvedBuild\Release\wxl-world-invert.dll"
Write-Host 'No live client/profile files were modified or deployed.'
