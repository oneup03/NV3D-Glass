<#
.SYNOPSIS
    Build NV3D-Glass.sln from PowerShell without needing a Developer Command
    Prompt. Used by .vscode/tasks.json and convenient for the command line.

    msbuild.exe is not on PATH in a normal PowerShell session - it ships
    inside the VS 2022 install tree. This script locates it via vswhere
    (the same pattern external/NV3D-Lib/tools/build.ps1 uses for the
    submodule), then runs the build.

.PARAMETER Configuration
    Release-MT | Debug-MT | Release-MD | Debug-MD. Default: Release-MT.

.PARAMETER Platform
    x64 | x86. Default: x64. (Only x64 is shipped; the others exist for parity.)

.PARAMETER Target
    MSBuild target. Default: Build. Use Rebuild / Clean as needed.

.EXAMPLE
    .\tools\build.ps1
    .\tools\build.ps1 -Target Rebuild
    .\tools\build.ps1 -Configuration Debug-MT
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug-MT','Release-MT','Debug-MD','Release-MD')]
    [string]$Configuration = 'Release-MT',

    [ValidateSet('x86','x64')]
    [string]$Platform = 'x64',

    [string]$Target = 'Build'
)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Install Visual Studio 2022 (Community / Build Tools) with the C++ workload."
}

$msbuild = & $vswhere -latest -prerelease `
    -requires Microsoft.Component.MSBuild `
    -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1

if (-not $msbuild -or -not (Test-Path $msbuild)) {
    throw "MSBuild.exe not located via vswhere. Ensure the C++ build tools workload is installed."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $repoRoot 'NV3D-Glass.sln'
if (-not (Test-Path $solution)) { throw "Solution not found: $solution" }

Write-Host "MSBuild   : $msbuild"
Write-Host "Solution  : $solution"
Write-Host "Target    : $Target"
Write-Host "Config    : $Configuration|$Platform"
Write-Host ""

& $msbuild $solution `
    "/t:$Target" `
    "/p:Configuration=$Configuration" `
    "/p:Platform=$Platform" `
    /m /nologo /v:minimal /clp:Summary

if ($LASTEXITCODE -ne 0) {
    throw "Build failed (exit $LASTEXITCODE) for $Configuration|$Platform (target=$Target)."
}
