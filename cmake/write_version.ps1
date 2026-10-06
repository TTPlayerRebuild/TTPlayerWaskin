[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Template,
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$build = Get-WaskinBuildVersion $Version
$fixed = '{0},{1},{2},{3}' -f $build.Date.Year,$build.Date.Month,$build.Date.Day,$build.Patch
$content = (Get-Content -LiteralPath $Template -Encoding UTF8 -Raw).
    Replace('@WASKIN_FIXED_VERSION@', $fixed).Replace('@WASKIN_BUILD_VERSION@', $build.Name)
if ($content -match '@[A-Z_]+@') { throw 'Unresolved version resource placeholder.' }
New-Item -ItemType Directory -Path (Split-Path $OutputPath -Parent) -Force | Out-Null
# Run every build to refresh automatic dates, but preserve incremental builds.
if (-not (Test-Path -LiteralPath $OutputPath) -or
    [IO.File]::ReadAllText($OutputPath) -cne $content) {
    [IO.File]::WriteAllText($OutputPath, $content, (New-Object Text.UTF8Encoding($false)))
}
