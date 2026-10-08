[CmdletBinding()]
param([string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build\default\Debug'))
$ErrorActionPreference = 'Stop'
$executable = (Resolve-Path (Join-Path $BuildDirectory 'veritassync-local-peer.exe')).Path
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('veritassync-e2e-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$processes = [Collections.Generic.List[Diagnostics.Process]]::new()
function Request([string]$pipe, [string]$command, [string[]]$arguments = @()) {
  $client = [IO.Pipes.NamedPipeClientStream]::new('.', $pipe, [IO.Pipes.PipeDirection]::InOut)
  try {
    $client.Connect(1500)
    $client.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    $message = "VSYNC_IPC/1`t$command"
    foreach ($argument in $arguments) { $message += "`t" + $argument.Replace('%','%25').Replace("`t",'%09').Replace("`n",'%0A').Replace("`r",'%0D') }
    $bytes = [Text.Encoding]::UTF8.GetBytes($message)
    $client.Write($bytes, 0, $bytes.Length)
    $result = [IO.MemoryStream]::new()
    do { $buffer = [byte[]]::new(65536); $count = $client.Read($buffer, 0, $buffer.Length); $result.Write($buffer, 0, $count) } while (-not $client.IsMessageComplete)
    $reply = [Text.Encoding]::UTF8.GetString($result.ToArray())
    $result.Dispose()
    if ($reply.StartsWith('ERR')) { throw $reply }
    return $reply
  } finally { $client.Dispose() }
}
function Wait-For([scriptblock]$condition, [string]$description) {
  $deadline = [DateTime]::UtcNow.AddSeconds(25)
  do {
    try { if (& $condition) { return } } catch { }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "Timed out: $description. Diagnostic logs retained in $testRoot"
}
function Write-TestFile([string]$path, [string]$content) { [IO.File]::WriteAllText($path, $content, [Text.UTF8Encoding]::new($false)) }
function Equal-Files([string]$left, [string]$right) {
  return (Test-Path -LiteralPath $left -PathType Leaf) -and (Test-Path -LiteralPath $right -PathType Leaf) -and
    (Get-FileHash -LiteralPath $left).Hash -eq (Get-FileHash -LiteralPath $right).Hash
}
function Start-Peer([string]$mode, [string]$role, [string]$device, [string]$peer, [string]$root, [string]$pipe, [int]$port, [bool]$listen, [int]$SendBudget = 0) {
  $arguments = @('--mode', $mode, '--role', $role, '--device', $device, '--peer', $peer,
    '--root', $root, '--db', (Join-Path $testRoot "$device.db"), '--pipe', "\\.\pipe\$pipe", '--port', $port)
  if ($listen) { $arguments += '--listen' }
  if ($SendBudget -gt 0) { $arguments += @('--send-budget', $SendBudget) }
  $process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardError (Join-Path $testRoot "$device-$([guid]::NewGuid().ToString('N')).log")
  $processes.Add($process)
  Wait-For { (Request $pipe 'ping').StartsWith('OK') } "$device IPC ready"
  return $process
}
function Free-Port {
  $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
  $listener.Start()
  $port = $listener.LocalEndpoint.Port
  $listener.Stop()
  return $port
}
try {
  $left = Join-Path $testRoot 'left'; $right = Join-Path $testRoot 'right'
  New-Item -ItemType Directory -Path $left,$right | Out-Null
  $nonce = [guid]::NewGuid().ToString('N'); $leftPipe = "vsync-e2e-a-$nonce"; $rightPipe = "vsync-e2e-b-$nonce"
  Write-TestFile (Join-Path $left 'shared.txt') 'initial'
  $port = Free-Port
  $a = Start-Peer 'bidirectional' 'peer' 'a' 'b' $left $leftPipe $port $true
  $b = Start-Peer 'bidirectional' 'peer' 'b' 'a' $right $rightPipe $port $false
  Wait-For { Equal-Files (Join-Path $left 'shared.txt') (Join-Path $right 'shared.txt') } 'initial bidirectional copy'
  Write-TestFile (Join-Path $right 'shared.txt') 'automatic-watcher-edit'
  Wait-For { Equal-Files (Join-Path $left 'shared.txt') (Join-Path $right 'shared.txt') } 'reverse watcher edit'
  [void](Request $rightPipe 'pause_task' @('local'))
  Write-TestFile (Join-Path $right 'paused.txt') 'paused'
  Start-Sleep -Milliseconds 700
  if (Test-Path (Join-Path $left 'paused.txt')) { throw 'Paused task transferred a new file' }
  [void](Request $rightPipe 'resume_task' @('local'))
  Wait-For { Equal-Files (Join-Path $left 'paused.txt') (Join-Path $right 'paused.txt') } 'pause/resume'
  $policy = Request $leftPipe 'ignore_get' @('local')
  $hash = ($policy.TrimEnd() -split "`t")[2]
  [void](Request $leftPipe 'ignore_apply' @('local', $hash, "*.log`n", 'manual'))
  Wait-For { Equal-Files (Join-Path $left '.veritasignore') (Join-Path $right '.veritasignore') } 'negotiated ignore rules'
  Write-TestFile (Join-Path $right 'private.log') 'keep-local'
  Write-TestFile (Join-Path $right 'visible.txt') 'sync'
  Wait-For { Equal-Files (Join-Path $left 'visible.txt') (Join-Path $right 'visible.txt') } 'sync after rule agreement'
  if (Test-Path (Join-Path $left 'private.log')) { throw 'Negotiated ignored file transferred' }
  $policy = Request $leftPipe 'ignore_get' @('local')
  $hash = ($policy.TrimEnd() -split "`t")[2]
  [void](Request $leftPipe 'ignore_undo' @('local', $hash))
  Wait-For { Equal-Files (Join-Path $left 'private.log') (Join-Path $right 'private.log') } 'negotiated ignore undo'
  [void](Request $leftPipe 'shutdown'); [void](Request $rightPipe 'shutdown')
  $a.WaitForExit(5000) | Out-Null; $b.WaitForExit(5000) | Out-Null
  Write-TestFile (Join-Path $left 'shared.txt') 'offline-left'
  Write-TestFile (Join-Path $right 'shared.txt') 'offline-right'
  $a = Start-Peer 'bidirectional' 'peer' 'a' 'b' $left $leftPipe $port $true
  $b = Start-Peer 'bidirectional' 'peer' 'b' 'a' $right $rightPipe $port $false
  Wait-For {
    (Equal-Files (Join-Path $left 'shared.txt') (Join-Path $right 'shared.txt')) -and
      @(Get-ChildItem -LiteralPath $left -Filter '*.conflict.*').Count -gt 0 -and
      @(Get-ChildItem -LiteralPath $right -Filter '*.conflict.*').Count -gt 0
  } 'restart and offline conflict preservation'
  Remove-Item -LiteralPath (Join-Path $left 'paused.txt')
  Wait-For { -not (Test-Path (Join-Path $right 'paused.txt')) } 'bidirectional deletion'
  [void](Request $leftPipe 'shutdown'); [void](Request $rightPipe 'shutdown')
  $a.WaitForExit(5000) | Out-Null; $b.WaitForExit(5000) | Out-Null
  Write-Output 'PASS: two-process bidirectional copy, watcher, pause/resume, policy agreement/undo, restart, conflict, deletion'

  $source = Join-Path $testRoot 'source-同步🙂'; $target = Join-Path $testRoot 'target-同步🙂'
  New-Item -ItemType Directory -Path $source,$target | Out-Null
  $bytes = [byte[]]::new(12 * 1024 * 1024 + 7)
  [Random]::new(42).NextBytes($bytes)
  [IO.File]::WriteAllBytes((Join-Path $source 'large.bin'), $bytes)
  Write-TestFile (Join-Path $source 'empty.txt') ''
  Write-TestFile (Join-Path $source '照片-你好🙂.txt') 'unicode round trip'
  Write-TestFile (Join-Path $source '.veritasignore') "*.log`n"
  Write-TestFile (Join-Path $source 'ignored.log') 'ignored'
  $port = Free-Port; $sourcePipe = "vsync-e2e-source-$nonce"; $targetPipe = "vsync-e2e-target-$nonce"
  $s = Start-Peer 'one_way' 'source' 'source' 'target' $source $sourcePipe $port $true 32768
  $t = Start-Peer 'one_way' 'target' 'target' 'source' $target $targetPipe $port $false
  Wait-For {
    $partial = Get-Item -LiteralPath (Join-Path $target 'large.bin.part') -ErrorAction SilentlyContinue
    $partial -and $partial.Length -ge 3 * 1024 * 1024 -and $partial.Length -lt $bytes.Length
  } 'in-flight large file with flushed chunk batches'
  $t.Kill(); $t.WaitForExit()
  if (Test-Path -LiteralPath (Join-Path $target 'large.bin')) { throw 'Crash was not mid-transfer' }
  $t = Start-Peer 'one_way' 'target' 'target' 'source' $target $targetPipe $port $false
  Wait-For { Equal-Files (Join-Path $source 'large.bin') (Join-Path $target 'large.bin') } 'large file hash match'
  if (Test-Path -LiteralPath (Join-Path $target 'large.bin.part')) { throw 'Committed download left a partial file' }
  Wait-For { Test-Path (Join-Path $target 'empty.txt') } 'empty file'
  Wait-For { Equal-Files (Join-Path $source '照片-你好🙂.txt') (Join-Path $target '照片-你好🙂.txt') } 'Unicode filename'
  if (Test-Path (Join-Path $target 'ignored.log')) { throw 'Ignored file transferred' }
  $t.Kill(); $t.WaitForExit()
  Write-TestFile (Join-Path $source 'after-crash.txt') 'recover'
  $t = Start-Peer 'one_way' 'target' 'target' 'source' $target $targetPipe $port $false
  Wait-For { Equal-Files (Join-Path $source 'after-crash.txt') (Join-Path $target 'after-crash.txt') } 'target crash recovery'
  Remove-Item -LiteralPath (Join-Path $source 'empty.txt')
  Wait-For { -not (Test-Path (Join-Path $target 'empty.txt')) } 'one-way deletion'
  [void](Request $sourcePipe 'shutdown'); [void](Request $targetPipe 'shutdown')
  Write-Output 'PASS: two-process one-way large/empty/Unicode files, in-flight crash/resume, ignore, restart, deletion'
  Write-Output "Local integration artifacts: $testRoot"
} finally {
  foreach ($process in $processes) {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
  }
}
