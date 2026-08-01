[CmdletBinding()]
param(
  [string]$TargetTriple = "x86_64-pc-windows-msvc",
  [switch]$AllowUntrustedCertificate
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
$signTool = (Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source)
if ($null -eq $signTool) {
  $sdkRoots = @(
    "C:\Program Files (x86)\Windows Kits\10\bin",
    "C:\Program Files\Windows Kits\10\bin"
  )
  $signTool = $sdkRoots |
    Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem -LiteralPath $_ -Recurse -Filter "signtool.exe" -File -ErrorAction SilentlyContinue } |
    Where-Object { $_.Directory.Name -eq "x64" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($signTool)) {
  throw "signtool.exe was not found. Install the Windows SDK signing tools or add signtool.exe to PATH."
}

function Sign-And-Verify([string]$Path, [string]$CertificatePath) {
  & $signTool sign /fd SHA256 /f $CertificatePath /p $env:WINDOWS_CERTIFICATE_PASSWORD /tr "http://timestamp.digicert.com" /td SHA256 $Path
  if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $Path" }
  if ($AllowUntrustedCertificate) {
    $signature = Get-AuthenticodeSignature -FilePath $Path
    if ($signature.SignatureType -ne "Authenticode" -or $null -eq $signature.SignerCertificate) {
      throw "Self-signed Authenticode signature was not embedded: $Path"
    }
    return
  }
  & $signTool verify /pa /v $Path
  if ($LASTEXITCODE -ne 0) { throw "Authenticode verification failed: $Path" }
}

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
    $certificate = Join-Path $env:TEMP "veritassync-release.pfx"
    [IO.File]::WriteAllBytes($certificate, [Convert]::FromBase64String([Environment]::GetEnvironmentVariable("WINDOWS_CERTIFICATE_PFX_BASE64")))
    $stagedSidecars = @(
      "desktop\src-tauri\binaries\veritassync-engine-$TargetTriple.exe",
      "desktop\src-tauri\resources\engine\veritassync-engine.exe"
    )
    $stagedSidecars += Get-ChildItem "desktop\src-tauri\resources\engine" -Filter "*.dll" -File | Select-Object -ExpandProperty FullName
    foreach ($sidecar in $stagedSidecars) {
      if (-not (Test-Path -LiteralPath $sidecar -PathType Leaf)) { throw "Staged sidecar not found: $sidecar" }
      Sign-And-Verify $sidecar $certificate
    }

    Push-Location "desktop\src-tauri"
    cargo tauri build --config tauri.release.generated.json
    if ($LASTEXITCODE -ne 0) { throw "Tauri build failed" }
    Pop-Location

    $releaseFiles = @("desktop\src-tauri\target\release\veritassync-desktop.exe")
    $releaseFiles += Get-ChildItem "desktop\src-tauri\target\release\bundle" -Recurse -Include *.exe,*.msi | Select-Object -ExpandProperty FullName
    foreach ($releaseFile in ($releaseFiles | Select-Object -Unique)) {
      Sign-And-Verify $releaseFile $certificate
    }
  } finally {
    if ((Get-Location).Path -ne $repo) { Pop-Location }
    Remove-Item -LiteralPath $generated -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $certificate -Force -ErrorAction SilentlyContinue
  }
} finally {
  Pop-Location
}
