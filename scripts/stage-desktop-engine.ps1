[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build\default\Debug"),
  [string]$TargetTriple = "x86_64-pc-windows-msvc"
)

$engine = Join-Path $BuildDirectory "veritassync-engine.exe"
if (-not (Test-Path -LiteralPath $engine -PathType Leaf)) {
  throw "Engine executable was not found at $engine. Run 'cmake --build --preset default' first."
}
$destinationDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\binaries"
New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
$destination = Join-Path $destinationDirectory ("veritassync-engine-" + $TargetTriple + ".exe")
Copy-Item -LiteralPath $engine -Destination $destination -Force
$resourceDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\resources\engine"
New-Item -ItemType Directory -Force -Path $resourceDirectory | Out-Null
$resourceEngine = Join-Path $resourceDirectory "veritassync-engine.exe"
Copy-Item -LiteralPath $engine -Destination $resourceEngine -Force
$developmentResourceDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\target\debug\resources\engine"
New-Item -ItemType Directory -Force -Path $developmentResourceDirectory | Out-Null
Copy-Item -LiteralPath $engine -Destination (Join-Path $developmentResourceDirectory "veritassync-engine.exe") -Force
$runtimeLibraries = Get-ChildItem -LiteralPath $BuildDirectory -Filter "*.dll" -File
foreach ($library in $runtimeLibraries) {
  Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $resourceDirectory $library.Name) -Force
  Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $developmentResourceDirectory $library.Name) -Force
}
Write-Host "Staged engine sidecar: $destination"
Write-Host "Staged engine runtime resource: $resourceEngine"
Write-Host "Staged $($runtimeLibraries.Count) engine runtime DLL(s) into: $resourceDirectory"
Write-Host "Refreshed debug runtime resource: $developmentResourceDirectory"
