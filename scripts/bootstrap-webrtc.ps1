param(
  [Parameter(Mandatory = $true)] [string] $CheckoutRoot
)

$ErrorActionPreference = 'Stop'
$lockPath = Join-Path $PSScriptRoot '..\third_party\libwebrtc.lock'
$lock = Get-Content -Raw $lockPath
if ($lock -notmatch 'commit = "([0-9a-f]{40})"') { throw "Invalid libwebrtc lock file: $lockPath" }
$commit = $Matches[1]

if (-not (Get-Command fetch -ErrorAction SilentlyContinue) -or -not (Get-Command gclient -ErrorAction SilentlyContinue)) {
  throw 'depot_tools is required. Put depot_tools on PATH, then rerun this script.'
}

New-Item -ItemType Directory -Force -Path $CheckoutRoot | Out-Null
Push-Location $CheckoutRoot
try {
  if (-not (Test-Path 'src\.git')) { fetch --nohooks webrtc }
  Push-Location src
  try {
    git checkout $commit
    gclient sync -D
    Write-Host "Pinned libwebrtc checkout is ready at $PWD"
  } finally { Pop-Location }
} finally { Pop-Location }
