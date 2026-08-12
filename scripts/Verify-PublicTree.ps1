[CmdletBinding()]
param(
    [string]$Root = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'
$resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
$forbiddenDirectories = @('artifacts', 'backups', '.build', '.wxl-sdk', 'WTF', 'Data')
$forbiddenExtensions = @('.dll', '.exe', '.pdb', '.lib', '.obj', '.mpq', '.m2', '.mdx', '.wmo', '.blp', '.dbc', '.db2')
$violations = New-Object System.Collections.Generic.List[string]
$isGitRepository = Test-Path -LiteralPath (Join-Path $resolvedRoot '.git')

Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Force | ForEach-Object {
    $relative = $_.FullName.Substring($resolvedRoot.Length).TrimStart('\')
    if ($relative -eq '.git' -or $relative.StartsWith('.git\')) { return }
    if ($isGitRepository) {
        & git -C $resolvedRoot check-ignore -q -- $relative
        if ($LASTEXITCODE -eq 0) { return }
    }
    $segments = $relative -split '\\'
    if ($segments | Where-Object { $forbiddenDirectories -contains $_ }) {
        $violations.Add("forbidden directory content: $relative")
        return
    }
    if (-not $_.PSIsContainer -and $forbiddenExtensions -contains $_.Extension.ToLowerInvariant()) {
        $violations.Add("forbidden binary or game asset: $relative")
    }
}

if ($violations.Count -gt 0) {
    $violations | Sort-Object | ForEach-Object { Write-Error $_ }
    throw 'Public-tree verification failed.'
}

$global:LASTEXITCODE = 0
Write-Host 'PUBLIC_TREE_OK'
