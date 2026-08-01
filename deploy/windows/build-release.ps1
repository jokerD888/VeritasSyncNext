[CmdletBinding()]
param(
  [string]$TargetTriple = "x86_64-pc-windows-msvc"
)

$required = @(
  "WINDOWS_CERTIFICATE_PFX_BASE64",
  "WINDOWS_CERTIFICATE_PASSWORD",
  "TAURI_SIGNING_PRIVATE_KEY",
  "TAURI_SIGNING_PRIVATE_KEY_PASSWORD",
  "VERITASSYNC_UPDATE_ENDPOINT",
  "VERITASSYNC_UPDATER_PUBKEY"
)
foreach ($name in $required) {
  if ([string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable($name))) {
    throw "Missing required release secret: $name"
  }
}

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Push-Location $repo
try {
  cmake --preset default
  if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
  cmake --build --preset default
  if ($LASTEXITCODE -ne 0) { throw "Engine build failed" }
  ctest --preset default --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "Engine tests failed" }
  & (Join-Path $repo "scripts\stage-desktop-engine.ps1") -TargetTriple $TargetTriple
  $template = Get-Content -LiteralPath "desktop\src-tauri\tauri.release.example.json" -Raw
  $releaseConfig = $template.Replace("__VERITASSYNC_UPDATE_ENDPOINT__", [Environment]::GetEnvironmentVariable("VERITASSYNC_UPDATE_ENDPOINT"))
  $releaseConfig = $releaseConfig.Replace("__VERITASSYNC_UPDATER_PUBKEY__", [Environment]::GetEnvironmentVariable("VERITASSYNC_UPDATER_PUBKEY"))
  $generated = "desktop\src-tauri\tauri.release.generated.json"
  Set-Content -LiteralPath $generated -Value $releaseConfig -Encoding utf8NoBOM
  try {
    Push-Location "desktop\src-tauri"
    cargo tauri build --config tauri.release.generated.json
    if ($LASTEXITCODE -ne 0) { throw "Tauri build failed" }
  } finally {
    Pop-Location
    Remove-Item -LiteralPath $generated -Force -ErrorAction SilentlyContinue
  }
  $certificate = Join-Path $env:TEMP "veritassync-release.pfx"
  try {
    [IO.File]::WriteAllBytes($certificate, [Convert]::FromBase64String([Environment]::GetEnvironmentVariable("WINDOWS_CERTIFICATE_PFX_BASE64")))
    Get-ChildItem "desktop\src-tauri\target\release\bundle" -Recurse -Include *.exe,*.msi | ForEach-Object {
      & signtool sign /fd SHA256 /f $certificate /p $env:WINDOWS_CERTIFICATE_PASSWORD /tr "http://timestamp.digicert.com" /td SHA256 $_.FullName
      if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $($_.FullName)" }
      & signtool verify /pa /v $_.FullName
      if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $($_.FullName)" }
    }
  } finally {
    Remove-Item -LiteralPath $certificate -Force -ErrorAction SilentlyContinue
  }
} finally {
  Pop-Location
}
