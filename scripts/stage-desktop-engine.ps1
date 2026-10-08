[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot "..\build\default\Debug"),
  [string]$TargetTriple = "x86_64-pc-windows-msvc",
  [string]$WebRtcBridge,
  [switch]$SkipDevelopmentResources
)

$engine = Join-Path $BuildDirectory "veritassync-engine.exe"
if (-not (Test-Path -LiteralPath $engine -PathType Leaf)) {
  throw "Engine executable was not found at $engine. Run 'cmake --build --preset default' first."
}
if (-not [string]::IsNullOrWhiteSpace($WebRtcBridge) -and
    -not (Test-Path -LiteralPath $WebRtcBridge -PathType Leaf)) {
  throw "WebRTC bridge was not found: $WebRtcBridge"
}
$destinationDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\binaries"
New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
$sidecarDestination = Join-Path $destinationDirectory ("veritassync-engine-" + $TargetTriple + ".exe")
Copy-Item -LiteralPath $engine -Destination $sidecarDestination -Force
$resourceDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\resources\engine"
New-Item -ItemType Directory -Force -Path $resourceDirectory | Out-Null
$resourceEngine = Join-Path $resourceDirectory "veritassync-engine.exe"
Copy-Item -LiteralPath $engine -Destination $resourceEngine -Force
$developmentResourceDirectory = Join-Path $PSScriptRoot "..\desktop\src-tauri\target\debug\resources\engine"
if (-not $SkipDevelopmentResources) {
  New-Item -ItemType Directory -Force -Path $developmentResourceDirectory | Out-Null
  Copy-Item -LiteralPath $engine -Destination (Join-Path $developmentResourceDirectory "veritassync-engine.exe") -Force
}
$runtimeLibraries = Get-ChildItem -LiteralPath $BuildDirectory -Filter "*.dll" -File
foreach ($library in $runtimeLibraries) {
  Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $resourceDirectory $library.Name) -Force
  if (-not $SkipDevelopmentResources) {
    Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $developmentResourceDirectory $library.Name) -Force
  }
}
if (-not [string]::IsNullOrWhiteSpace($WebRtcBridge)) {
  $bridgeDestinations = @($resourceDirectory)
  if (-not $SkipDevelopmentResources) { $bridgeDestinations += $developmentResourceDirectory }
  foreach ($bridgeDestination in $bridgeDestinations) {
    Copy-Item -LiteralPath $WebRtcBridge -Destination (Join-Path $bridgeDestination "veritassync_webrtc_bridge.dll") -Force
  }
  Write-Host "Staged WebRTC bridge: $WebRtcBridge"
}
Write-Host "Staged engine sidecar: $sidecarDestination"
Write-Host "Staged engine runtime resource: $resourceEngine"
Write-Host "Staged $($runtimeLibraries.Count) engine runtime DLL(s) into: $resourceDirectory"
if (-not $SkipDevelopmentResources) {
  Write-Host "Refreshed debug runtime resource: $developmentResourceDirectory"
}
