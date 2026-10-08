[CmdletBinding()]
param(
  [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\default\Release'),
  [string]$TrackerExecutable = (Join-Path $PSScriptRoot '..\tracker\target\debug\veritassync-tracker.exe'),
  [Parameter(Mandatory)][string]$WebRtcBridge,
  [ValidateRange(0, 3600)][int]$SoakSeconds = 30,
  [switch]$NativeDiagnostics,
  [switch]$MultiTargetOnly,
  [ValidateRange(0, 65535)][int]$LocalTurnPort = 0,
  [ValidateSet('udp', 'tcp')][string]$LocalTurnTransport = 'udp',
  [string]$LocalTurnCredential
)
$ErrorActionPreference = 'Stop'
$engine = (Resolve-Path (Join-Path $BuildDirectory 'veritassync-engine.exe')).Path
$tracker = (Resolve-Path $TrackerExecutable).Path
$bridge = (Resolve-Path $WebRtcBridge).Path
$nonce = [guid]::NewGuid().ToString('N')
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "veritassync-native-e2e-$nonce"
New-Item -ItemType Directory -Path $testRoot | Out-Null
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
$credentials = [Collections.Generic.List[string]]::new()
$savedEnvironment = @{}
foreach ($name in @('VERITASSYNC_WEBRTC_BRIDGE_LIBRARY', 'VERITASSYNC_ICE_URLS',
    'VERITASSYNC_ICE_RELAY_ONLY', 'VERITASSYNC_TURN_USERNAME', 'VERITASSYNC_TURN_CREDENTIAL', 'VERITASSYNC_WEBRTC_ALLOW_LOOPBACK',
    'VERITASSYNC_WEBRTC_DIAGNOSTICS', 'VERITASSYNC_TRACKER_BIND', 'VERITASSYNC_TRACKER_DB')) {
  $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
function Request([string]$pipe, [string]$command, [string[]]$arguments = @()) {
  $client = [IO.Pipes.NamedPipeClientStream]::new('.', $pipe, [IO.Pipes.PipeDirection]::InOut)
  try {
    $client.Connect(2000)
    $client.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    $message = "VSYNC_IPC/1`t$command"
    foreach ($argument in $arguments) {
      $message += "`t" + $argument.Replace('%','%25').Replace("`t",'%09').Replace("`n",'%0A').Replace("`r",'%0D')
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes($message)
    $client.Write($bytes, 0, $bytes.Length)
    $result = [IO.MemoryStream]::new()
    try {
      do {
        $buffer = [byte[]]::new(65536)
        $count = $client.Read($buffer, 0, $buffer.Length)
        if ($count -eq 0) { throw 'Engine closed IPC without a reply' }
        $result.Write($buffer, 0, $count)
      } while (-not $client.IsMessageComplete)
      $reply = [Text.Encoding]::UTF8.GetString($result.ToArray())
    } finally { $result.Dispose() }
    if ($reply.StartsWith('ERR')) { throw $reply }
    return $reply
  } finally { $client.Dispose() }
}
function Wait-For([scriptblock]$condition, [string]$description, [int]$seconds = 90) {
  $deadline = [DateTime]::UtcNow.AddSeconds($seconds)
  $lastError = ''
  do {
    try { if (& $condition) { return } } catch { $lastError = $_.Exception.Message }
    Start-Sleep -Milliseconds 50
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "Timed out: $description ($lastError). Logs: $testRoot"
}
function Write-TestFile([string]$path, [string]$content) {
  [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false))
}
function Equal-Files([string]$left, [string]$right) {
  return (Test-Path -LiteralPath $left -PathType Leaf) -and
    (Test-Path -LiteralPath $right -PathType Leaf) -and
    (Get-FileHash -LiteralPath $left).Hash -eq (Get-FileHash -LiteralPath $right).Hash
}
function Start-Engine([string]$name) {
  $credential = "VeritasSyncNext/NativeTest/$nonce/$name"
  if (-not $credentials.Contains($credential)) { $credentials.Add($credential) }
  $pipe = "vsync-native-$nonce-$name"
  $launch = [guid]::NewGuid().ToString('N')
  $arguments = @('--ipc-serve', '--db', ('"' + (Join-Path $testRoot "$name.db") + '"'),
    '--pipe', "\\.\pipe\$pipe", '--identity-target', $credential)
  $process = Start-Process -FilePath $engine -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardError (Join-Path $testRoot "$name-$launch.err.log") `
    -RedirectStandardOutput (Join-Path $testRoot "$name-$launch.out.log")
  $processes.Add($process)
  Wait-For { (Request $pipe 'ping').StartsWith('OK') } "$name engine ready" 15
  return $process
}
function Pipe([string]$name) { return "vsync-native-$nonce-$name" }
function Invitation([string]$name, [string]$task) {
  $field = ((Request (Pipe $name) 'create_invitation' @($task, $trackerUrl)).TrimEnd() -split "`t")[1]
  return $field.Replace('%09', "`t").Replace('%0A', "`n").Replace('%0D', "`r").Replace('%25', '%')
}
function Metrics([string]$name, [string]$task) {
  $rows = (Request (Pipe $name) 'dashboard' @('10')) -split "`n"
  return @($rows | Where-Object { $_.StartsWith("METRICS`t$task`t") })[0] -split "`t"
}
function Stop-Engine([string]$name, [Diagnostics.Process]$process) {
  [void](Request (Pipe $name) 'shutdown')
  if (-not $process.WaitForExit(10000)) { throw "$name did not shut down" }
  if ($process.ExitCode -ne 0) { throw "$name exited with $($process.ExitCode)" }
}
function Start-Tracker {
  $launch = [guid]::NewGuid().ToString('N')
  $process = Start-Process -FilePath $tracker -WindowStyle Hidden -PassThru `
    -RedirectStandardError (Join-Path $testRoot "tracker-$launch.err.log") `
    -RedirectStandardOutput (Join-Path $testRoot "tracker-$launch.out.log")
  $processes.Add($process)
  Wait-For { (Invoke-WebRequest "$trackerUrl/healthz" -TimeoutSec 2).Content.Trim() -eq 'ok' } 'Tracker ready' 15
  return $process
}
try {
  # Process-local overrides: host candidates only, no STUN/TURN or external service.
  $env:VERITASSYNC_WEBRTC_BRIDGE_LIBRARY = $bridge
  $env:VERITASSYNC_ICE_URLS = ''
  $env:VERITASSYNC_ICE_RELAY_ONLY = '0'
  if ($LocalTurnPort -gt 0) {
    if ([string]::IsNullOrWhiteSpace($LocalTurnCredential)) { throw 'LocalTurnCredential is required for local TURN tests' }
    $env:VERITASSYNC_ICE_URLS = "turn:127.0.0.1:${LocalTurnPort}?transport=$LocalTurnTransport"
    $env:VERITASSYNC_TURN_USERNAME = 'native-test'
    $env:VERITASSYNC_TURN_CREDENTIAL = $LocalTurnCredential
    $env:VERITASSYNC_ICE_RELAY_ONLY = '1'
    # A TURN endpoint on 127.0.0.1 requires a loopback source interface too.
    # This opt-in bridge diagnostic mode never changes normal production defaults.
    Write-Output "Local TURN relay-only test: $LocalTurnTransport on 127.0.0.1:$LocalTurnPort"
  }
  $env:VERITASSYNC_WEBRTC_ALLOW_LOOPBACK = ''
  if ($LocalTurnPort -gt 0) { $env:VERITASSYNC_WEBRTC_ALLOW_LOOPBACK = '1' }
  $env:VERITASSYNC_WEBRTC_DIAGNOSTICS = if ($NativeDiagnostics) { '1' } else { '' }
  $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
  $listener.Start()
  $port = $listener.LocalEndpoint.Port
  $listener.Stop()
  $trackerUrl = "http://127.0.0.1:$port"
  $env:VERITASSYNC_TRACKER_BIND = "127.0.0.1:$port"
  $env:VERITASSYNC_TRACKER_DB = Join-Path $testRoot 'tracker.db'
  $trackerProcess = Start-Tracker
  if (-not $MultiTargetOnly) {
  $left = Join-Path $testRoot 'left 同步🙂'; $right = Join-Path $testRoot 'right 同步🙂'
  New-Item -ItemType Directory -Path $left,$right | Out-Null
  Write-TestFile (Join-Path $left '照片-你好🙂.txt') 'initial'
  $a = Start-Engine 'a'; $b = Start-Engine 'b'
  $identityA = (Request (Pipe 'a') 'device_identity').TrimEnd()
  if ($identityA -eq (Request (Pipe 'b') 'device_identity').TrimEnd()) { throw 'Engines share a device identity' }
  [void](Request (Pipe 'a') 'create_task' @('native-bidir', 'bidirectional', 'peer', $left))
  $token = Invitation 'a' 'native-bidir'
  [void](Request (Pipe 'b') 'join_invitation' @($token, $right))
  Write-Output 'Native bidirectional: paired through live Tracker; waiting for real channel transfer'
  Wait-For { Equal-Files (Join-Path $left '照片-你好🙂.txt') (Join-Path $right '照片-你好🙂.txt') } 'native bidirectional initial transfer'
  Write-TestFile (Join-Path $right '照片-你好🙂.txt') 'reverse native watcher edit'
  Wait-For { Equal-Files (Join-Path $left '照片-你好🙂.txt') (Join-Path $right '照片-你好🙂.txt') } 'native reverse watcher edit'
  [void](Request (Pipe 'b') 'pause_task' @('native-bidir'))
  Write-TestFile (Join-Path $right 'paused.txt') 'resume over WebRTC'
  Start-Sleep -Milliseconds 1000
  if (Test-Path -LiteralPath (Join-Path $left 'paused.txt')) { throw 'Paused native task transferred a file' }
  [void](Request (Pipe 'b') 'resume_task' @('native-bidir'))
  Wait-For { Equal-Files (Join-Path $left 'paused.txt') (Join-Path $right 'paused.txt') } 'native pause/resume reconnect'
  $hash = ((Request (Pipe 'a') 'ignore_get' @('native-bidir')).TrimEnd() -split "`t")[2]
  [void](Request (Pipe 'a') 'ignore_apply' @('native-bidir', $hash, "*.log`n", 'manual'))
  Wait-For { Equal-Files (Join-Path $left '.veritasignore') (Join-Path $right '.veritasignore') } 'native policy agreement'
  Write-TestFile (Join-Path $right 'ignored.log') 'keep local'
  Write-TestFile (Join-Path $right 'visible.txt') 'visible'
  Wait-For { Equal-Files (Join-Path $left 'visible.txt') (Join-Path $right 'visible.txt') } 'native visible file'
  if (Test-Path -LiteralPath (Join-Path $left 'ignored.log')) { throw 'Native ignored file transferred' }
  $hash = ((Request (Pipe 'a') 'ignore_get' @('native-bidir')).TrimEnd() -split "`t")[2]
  [void](Request (Pipe 'a') 'ignore_undo' @('native-bidir', $hash))
  Wait-For { Equal-Files (Join-Path $left 'ignored.log') (Join-Path $right 'ignored.log') } 'native policy undo'
  Stop-Engine 'a' $a; Stop-Engine 'b' $b
  Write-TestFile (Join-Path $left '照片-你好🙂.txt') 'offline left'
  Write-TestFile (Join-Path $right '照片-你好🙂.txt') 'offline right'
  $a = Start-Engine 'a'; $b = Start-Engine 'b'
  if ($identityA -ne (Request (Pipe 'a') 'device_identity').TrimEnd()) { throw 'Restart changed persistent identity' }
  Wait-For {
    (Equal-Files (Join-Path $left '照片-你好🙂.txt') (Join-Path $right '照片-你好🙂.txt')) -and
    @(Get-ChildItem -LiteralPath $left -Filter '*.conflict.*').Count -gt 0 -and
    @(Get-ChildItem -LiteralPath $right -Filter '*.conflict.*').Count -gt 0
  } 'native restart and offline conflicts'
  Remove-Item -LiteralPath (Join-Path $left 'paused.txt')
  Wait-For { -not (Test-Path -LiteralPath (Join-Path $right 'paused.txt')) } 'native bidirectional deletion'
  Stop-Engine 'a' $a; Stop-Engine 'b' $b
  Write-Output 'PASS: live Tracker + formal engines + native bidirectional watcher, pause/resume, policy/undo, restart/conflicts/deletion'
  }

  $source = Join-Path $testRoot 'source 同步🙂'
  $target1 = Join-Path $testRoot 'target1 同步🙂'; $target2 = Join-Path $testRoot 'target2 同步🙂'
  New-Item -ItemType Directory -Path $source,$target1,$target2 | Out-Null
  Write-TestFile (Join-Path $source 'empty.txt') ''
  Write-TestFile (Join-Path $source '.veritasignore') "*.log`n"
  Write-TestFile (Join-Path $source 'ignored.log') 'ignored'
  $s = Start-Engine 'source'; $t1 = Start-Engine 'target1'; $t2 = Start-Engine 'target2'
  [void](Request (Pipe 'source') 'create_task' @('native-multi', 'one_way', 'source', $source))
  [void](Request (Pipe 'target1') 'join_invitation' @((Invitation 'source' 'native-multi'), $target1))
  Wait-For { (Metrics 'source' 'native-multi')[8] -eq '1' -and
    (Metrics 'target1' 'native-multi')[8] -eq '1' -and
    (Test-Path -LiteralPath (Join-Path $target1 'empty.txt')) } 'first target connected before second invitation'
  [void](Request (Pipe 'target2') 'join_invitation' @((Invitation 'source' 'native-multi'), $target2))
  Wait-For {
    (Metrics 'source' 'native-multi')[8] -eq '2' -and
    (Metrics 'target1' 'native-multi')[8] -eq '1' -and
    (Metrics 'target2' 'native-multi')[8] -eq '1' -and
    (Test-Path -LiteralPath (Join-Path $target1 'empty.txt')) -and
    (Test-Path -LiteralPath (Join-Path $target2 'empty.txt'))
  } 'two native targets connected only to source'
  if ((Get-Item -LiteralPath (Join-Path $target1 'empty.txt')).Length -ne 0) { throw 'Empty file is not empty' }
  $largeName = '大文件-🙂.bin'
  $bytes = [byte[]]::new(64 * 1024 * 1024 + 7)
  [Random]::new(42).NextBytes($bytes)
  [IO.File]::WriteAllBytes((Join-Path $source $largeName), $bytes)
  Wait-For {
    $partial = Get-Item -LiteralPath (Join-Path $target1 "$largeName.part") -ErrorAction SilentlyContinue
    $partial -and $partial.Length -ge 16 * 1024 * 1024 -and $partial.Length -lt $bytes.Length
  } 'native transfer reaches durable mid-file checkpoint'
  $t1.Kill(); $t1.WaitForExit()
  if (Test-Path -LiteralPath (Join-Path $target1 $largeName)) { throw 'Target crash was not mid-transfer' }
  Write-TestFile (Join-Path $source 'while-offline.txt') 'other target must keep receiving'
  Wait-For {
    (Equal-Files (Join-Path $source $largeName) (Join-Path $target2 $largeName)) -and
    (Equal-Files (Join-Path $source 'while-offline.txt') (Join-Path $target2 'while-offline.txt'))
  } 'healthy target continues while another target is crashed'
  $t1 = Start-Engine 'target1'
  Wait-For {
    (Equal-Files (Join-Path $source $largeName) (Join-Path $target1 $largeName)) -and
    (Equal-Files (Join-Path $source 'while-offline.txt') (Join-Path $target1 'while-offline.txt'))
  } 'native process-crash resume and offline catch-up'
  $received = [long](Metrics 'target1' 'native-multi')[3]
  if ($received -le 0 -or $received -ge [long]($bytes.Length * 0.9)) {
    throw "Resume did not save previously durable chunks: session received $received bytes"
  }
  if (Test-Path -LiteralPath (Join-Path $target1 "$largeName.part")) { throw 'Committed native download left .part' }
  if ((Test-Path -LiteralPath (Join-Path $target1 'ignored.log')) -or
      (Test-Path -LiteralPath (Join-Path $target2 'ignored.log'))) { throw 'One-way ignored file transferred' }
  Remove-Item -LiteralPath (Join-Path $source 'empty.txt')
  Wait-For { -not (Test-Path -LiteralPath (Join-Path $target1 'empty.txt')) -and
    -not (Test-Path -LiteralPath (Join-Path $target2 'empty.txt')) } 'native multi-target deletion'
  Write-Output "PASS: native multi-target topology/isolation, 64 MiB binary/Unicode/empty/ignore/deletion, crash resume ($received resumed-session bytes)"

  $s.Kill(); $s.WaitForExit()
  Write-TestFile (Join-Path $source 'source-restart.txt') 'written while source engine was stopped'
  $s = Start-Engine 'source'
  Wait-For { (Equal-Files (Join-Path $source 'source-restart.txt') (Join-Path $target1 'source-restart.txt')) -and
    (Equal-Files (Join-Path $source 'source-restart.txt') (Join-Path $target2 'source-restart.txt')) } 'native source-process crash recovery'
  $trackerProcess.Kill(); $trackerProcess.WaitForExit()
  Start-Sleep -Milliseconds 1500
  $trackerProcess = Start-Tracker
  Write-TestFile (Join-Path $source 'tracker-restart.txt') 'Tracker restored with durable room membership'
  Wait-For { (Equal-Files (Join-Path $source 'tracker-restart.txt') (Join-Path $target1 'tracker-restart.txt')) -and
    (Equal-Files (Join-Path $source 'tracker-restart.txt') (Join-Path $target2 'tracker-restart.txt')) } 'native Tracker service restart recovery'
  Write-Output 'PASS: native source-process and live Tracker process restart recovery'

  $soakDeadline = [DateTime]::UtcNow.AddSeconds($SoakSeconds)
  $soakStarted = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $iteration = 0
  while ([DateTime]::UtcNow -lt $soakDeadline) {
    Write-TestFile (Join-Path $source 'soak.txt') "iteration-$iteration"
    Wait-For { (Equal-Files (Join-Path $source 'soak.txt') (Join-Path $target1 'soak.txt')) -and
      (Equal-Files (Join-Path $source 'soak.txt') (Join-Path $target2 'soak.txt')) } 'native stability edits' 30
    foreach ($name in @('source','target1','target2')) {
      $dashboard = Request (Pipe $name) 'dashboard' @('30')
      foreach ($row in ($dashboard -split "`n")) {
        if ($row.StartsWith("EVENT`t")) {
          $fields = $row -split "`t"
          if ($fields[2] -eq 'native-multi' -and $fields[3] -eq 'error' -and [long]$fields[5] -ge $soakStarted) {
            throw "Unexpected error in $name dashboard during stability test"
          }
        }
      }
    }
    $iteration++
    if ($iteration % 10 -eq 0) { Write-Output "Native stability: $iteration revisions, $([int]($soakDeadline - [DateTime]::UtcNow).TotalSeconds) seconds remaining" }
    Start-Sleep -Milliseconds 500
  }
  Stop-Engine 'source' $s; Stop-Engine 'target1' $t1; Stop-Engine 'target2' $t2
  Write-Output "PASS: native bounded stability run ($SoakSeconds seconds, $iteration repeated revisions)"
  Write-Output "Native integration artifacts: $testRoot"
} finally {
  foreach ($process in $processes) {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
  }
  # Remove only the exact test credentials created by this run; never the default identity.
  foreach ($credential in $credentials) { & cmdkey.exe "/delete:$credential" | Out-Null }
  foreach ($name in $savedEnvironment.Keys) {
    [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
  }
  Write-Output "Logs and synthetic test files retained: $testRoot"
}
