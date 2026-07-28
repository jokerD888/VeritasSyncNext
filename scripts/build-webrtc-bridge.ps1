param(
  [Parameter(Mandatory = $true)] [string] $CheckoutRoot,
  [string] $OutputDirectory = 'out\veritassync'
)

$ErrorActionPreference = 'Stop'
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$sourceRoot = Join-Path $CheckoutRoot 'src'
$bridgeSource = Join-Path $PSScriptRoot '..\third_party\libwebrtc_bridge'
$bridgeDestination = Join-Path $sourceRoot 'veritassync_bridge'
$rootBuildPatch = Join-Path $bridgeSource 'root_build.patch'
$lockPath = Join-Path $PSScriptRoot '..\third_party\libwebrtc.lock'
$lock = Get-Content -Raw $lockPath
if ($lock -notmatch 'commit = "([0-9a-f]{40})"') { throw "Invalid libwebrtc lock file: $lockPath" }
$commit = $Matches[1]

if (-not (Test-Path $sourceRoot)) { throw "WebRTC source checkout is missing: $sourceRoot" }
if ((git -C $sourceRoot rev-parse HEAD).Trim() -ne $commit) { throw 'WebRTC checkout does not match third_party/libwebrtc.lock' }
if (-not (Get-Command gn -ErrorAction SilentlyContinue) -or -not (Get-Command autoninja -ErrorAction SilentlyContinue)) {
  throw 'depot_tools is required. Put depot_tools on PATH, then rerun this script.'
}

New-Item -ItemType Directory -Force -Path $bridgeDestination | Out-Null
Copy-Item -Path (Join-Path $bridgeSource '*') -Destination $bridgeDestination -Recurse -Force
Push-Location $sourceRoot
try {
  if (-not (Select-String -Path 'BUILD.gn' -SimpleMatch ':veritassync_webrtc_bridge' -Quiet)) {
    git apply $rootBuildPatch
    if ($LASTEXITCODE -ne 0) { throw 'Unable to add the bridge target to the pinned WebRTC build graph' }
  }
  gn gen $OutputDirectory
  if ($LASTEXITCODE -ne 0) { throw 'GN generation failed' }
  autoninja -C $OutputDirectory veritassync_webrtc_bridge
  if ($LASTEXITCODE -ne 0) { throw 'WebRTC bridge build failed' }
  Write-Host "WebRTC C ABI bridge is ready at $sourceRoot\$OutputDirectory\veritassync_webrtc_bridge.dll"
} finally { Pop-Location }
