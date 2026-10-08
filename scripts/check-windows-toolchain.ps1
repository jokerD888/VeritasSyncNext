[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
  throw 'Visual Studio Installer inventory is unavailable. The default CMake preset requires Visual Studio 2022 with x64 C++ tools.'
}
$inventory = & $vswhere -products '*' -version '[17.0,18.0)' -latest `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio 2022 inventory failed' }
$instances = @($inventory | ConvertFrom-Json)
if ($instances.Count -eq 0) {
  throw 'Visual Studio 2022 with x64 C++ tools was not found. CI must use windows-2022, matching the Visual Studio 17 2022 CMake preset; windows-latest can select a different toolchain.'
}
$instance = $instances[0]
$compilerSetup = Join-Path $instance.installationPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $compilerSetup -PathType Leaf)) {
  throw 'The selected Visual Studio 2022 instance is missing its x64 C++ environment setup.'
}
Write-Output "Verified Visual Studio 2022 ($($instance.installationVersion)): $($instance.installationPath)"
