[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\default\Release'),
  [Parameter(Mandatory)][string]$WebRtcBridge,
  [string]$TrackerExecutable = (Join-Path $PSScriptRoot '..\tracker\target\debug\veritassync-tracker.exe'),
  [string]$Distro = 'Ubuntu-22.04',
  [ValidateSet('udp', 'tcp')][string[]]$Transport = @('udp', 'tcp'),
  [ValidateRange(1024, 65535)][int]$ListenPort = 13478,
  [ValidateRange(1024, 65000)][int]$RelayPortStart = 14300,
  [ValidateRange(0, 3600)][int]$SoakSeconds = 10,
  [switch]$IncludeBidirectional
)
$ErrorActionPreference = 'Stop'
# Use the existing Docker daemon inside WSL. Never stop/change unrelated containers,
# install WSL, alter its networking mode, or add host firewall exceptions.
$name = 'veritassync-turn-test-' + [guid]::NewGuid().ToString('N')
$credential = [guid]::NewGuid().ToString('N')
$relayEnd = $RelayPortStart + 20
$started = $false
try {
  & wsl.exe -d $Distro -u root -- docker run --detach --rm --name $name --network host `
    coturn/coturn:4.6.3 -n --no-tls --no-dtls --fingerprint --lt-cred-mech `
    --realm=veritassync.local "--user=native-test:$credential" "--listening-port=$ListenPort" `
    --listening-ip=127.0.0.1 --relay-ip=127.0.0.1 "--min-port=$RelayPortStart" `
    "--max-port=$relayEnd" --allow-loopback-peers --no-cli --log-file=stdout
  if ($LASTEXITCODE -ne 0) { throw 'Cannot start the isolated local TURN container' }
  $started = $true
  foreach ($protocol in $Transport) {
    & (Join-Path $PSScriptRoot 'test-native-sync.ps1') -BuildDirectory $BuildDirectory `
      -TrackerExecutable $TrackerExecutable -WebRtcBridge $WebRtcBridge -SoakSeconds $SoakSeconds `
      -MultiTargetOnly:(-not $IncludeBidirectional) -LocalTurnPort $ListenPort `
      -LocalTurnTransport $protocol -LocalTurnCredential $credential
    if (-not $?) { throw "Local TURN/$protocol test failed" }
    Write-Output "PASS: local TURN/$protocol forced-relay acceptance"
  }
} finally {
  if ($started) {
    & wsl.exe -d $Distro -u root -- docker stop $name | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Warning "Could not stop test container $name; remove only this exact container manually." }
    else { Write-Output 'Removed the temporary TURN test container; downloaded coturn image retained for repeat tests.' }
  }
}
