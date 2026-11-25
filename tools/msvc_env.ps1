# SPDX-License-Identifier: GPL-2.0-only
# Configure MSVC x64 environment variables for this shell from project-local .tools\msvc.
$root = Split-Path -Parent $PSScriptRoot
$msvc = Join-Path $root '.tools\msvc'
if (-not (Test-Path (Join-Path $msvc 'setup_x64.bat'))) {
    throw "Project-local MSVC not found under $msvc"
}
$vcVer = (Get-ChildItem (Join-Path $msvc 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1).Name
$sdkVer = (Get-ChildItem (Join-Path $msvc 'Windows Kits\10\Include') -Directory | Sort-Object Name | Select-Object -Last 1).Name

$env:VCToolsVersion = $vcVer
$env:WindowsSDKVersion = "$sdkVer\"
$env:VCToolsInstallDir = Join-Path $msvc "VC\Tools\MSVC\$vcVer\"
$env:WindowsSdkBinPath = Join-Path $msvc 'Windows Kits\10\bin\'
$env:PATH = @(
    (Join-Path $msvc "VC\Tools\MSVC\$vcVer\bin\Hostx64\x64"),
    (Join-Path $msvc "Windows Kits\10\bin\$sdkVer\x64"),
    (Join-Path $msvc "Windows Kits\10\bin\$sdkVer\x64\ucrt"),
    $env:PATH
) -join ';'
$env:INCLUDE = @(
    (Join-Path $msvc "VC\Tools\MSVC\$vcVer\include"),
    (Join-Path $msvc "Windows Kits\10\Include\$sdkVer\ucrt"),
    (Join-Path $msvc "Windows Kits\10\Include\$sdkVer\shared"),
    (Join-Path $msvc "Windows Kits\10\Include\$sdkVer\um")
) -join ';'
$env:LIB = @(
    (Join-Path $msvc "VC\Tools\MSVC\$vcVer\lib\x64"),
    (Join-Path $msvc "Windows Kits\10\Lib\$sdkVer\ucrt\x64"),
    (Join-Path $msvc "Windows Kits\10\Lib\$sdkVer\um\x64")
) -join ';'
Write-Output "MSVC $vcVer + Windows SDK $sdkVer configured for this shell"
