param(
    [double]$Minutes = 10,             # total session length, including the boot phase
    [string]$Mode = 'wander',        # 'wander' or a timeline script (see src/autoplay.cpp)
    [int]$Seed = 1,                  # wander seed; same seed = same inputs
    [int]$BootSeconds = 45,          # A-tapping phase that gets through the menu and intro
    [string]$Name = '',              # session name (default: timestamp)
    [int]$CaptureEverySec = 10,      # screenshot interval for the contact sheet
    [int]$HangSeconds = 20,          # no guest frame for this long after boot = hang
    [switch]$PresentMon,             # also record display-side frame times
    [hashtable]$Env = @{},           # extra cvars as REX_<NAME> environment variables
    [string[]]$ExtraArgs = @()
)
# Unattended gameplay session: launches the game with in-process scripted input
# (--bully_autoplay), records frame times, CPU and memory, takes periodic screenshots,
# detects crashes and hangs, and writes logs/autoplay/<Name>/report.md.
# Input is injected inside the game, so the window does not need focus; keep it visible
# (not minimized), because a minimized window presents nothing.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'out\build\local-win-release\BullyRecomp.exe'
$map = Join-Path $root 'out\build\local-win-release\BullyRecomp.map'
if (-not $Name) { $Name = Get-Date -Format 'yyyyMMdd-HHmmss' }
$session = Join-Path $root "logs\autoplay\$Name"
Remove-Item -Recurse -Force $session -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $session 'captures') | Out-Null
if ($Mode -ne 'wander') { $Mode = (Resolve-Path $Mode).Path }

Get-Process BullyRecomp, PresentMon -ErrorAction SilentlyContinue | Stop-Process -Force
logman stop PresentMon -ets 2>&1 | Out-Null
Start-Sleep -Seconds 2

$perf = Join-Path $session 'perf.csv'
$launch = @('--game_data_root', ('"' + (Join-Path $root 'assets\game') + '"'),
    '--log_file', ('"' + (Join-Path $session 'game.log') + '"'), '--log_level', 'info',
    '--allow_game_relative_writes', '--mnk_mode',
    '--bully_perf_log', ('"' + $perf + '"'),
    '--bully_capture_dir', ('"' + (Join-Path $session 'captures') + '"'),
    '--bully_capture_interval_s', $CaptureEverySec, '--bully_capture_max', '100000',
    '--bully_autoplay', ('"' + $Mode + '"'), '--bully_autoplay_seed', $Seed,
    '--bully_autoplay_boot_s', $BootSeconds) + $ExtraArgs
$saved = @{}
foreach ($k in $Env.Keys) {
    $n = 'REX_' + $k.ToUpperInvariant()
    $saved[$n] = [Environment]::GetEnvironmentVariable($n, 'Process')
    [Environment]::SetEnvironmentVariable($n, [string]$Env[$k], 'Process')
}
$started = Get-Date
try {
    $game = Start-Process -FilePath $exe -ArgumentList $launch -WorkingDirectory $root -WindowStyle Normal -PassThru
} finally {
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n], 'Process') }
}
$pm = $null
if ($PresentMon) {
    $pm = Start-Process -FilePath (Join-Path $root 'tools\presentmon\PresentMon.exe') -WindowStyle Hidden -PassThru -ArgumentList @(
        '--process_id', $game.Id, '--output_file', ('"' + (Join-Path $session 'presentmon.csv') + '"'),
        '--delay', $BootSeconds, '--timed', [math]::Max(10, $Minutes * 60 - $BootSeconds - 5),
        '--terminate_after_timed', '--no_console_stats')
}
Write-Host "Autoplay session '$Name' running for $Minutes min (pid $($game.Id)); report: $session\report.md"

$outcome = 'completed'
$deadline = $started.AddMinutes($Minutes)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($game.HasExited) { $outcome = 'crashed'; break }
    # Hang: the per-second log keeps being written by the logger thread, but shows 0 frames.
    if (((Get-Date) - $started).TotalSeconds -gt $BootSeconds + $HangSeconds -and (Test-Path $perf)) {
        $tail = @(Get-Content $perf -Tail $HangSeconds -ErrorAction SilentlyContinue | Where-Object { $_ -notmatch '^t_s' })
        if ($tail.Count -ge $HangSeconds -and -not ($tail | Where-Object { [int](($_ -split ',')[1]) -gt 0 })) {
            $outcome = 'hung'; break
        }
    }
}
$ran = [math]::Round(((Get-Date) - $started).TotalSeconds)
$exitCode = $null; $faultRva = $null
if ($game.HasExited) {
    $exitCode = $game.ExitCode
    # Windows Error Reporting records the faulting offset; the report resolves it via the map.
    $event = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $started } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'BullyRecomp.exe' -and $_.Message -match ('0x{0:x}' -f $game.Id) } | Select-Object -First 1
    if ($event -and $event.Message -match 'Faulting module name: BullyRecomp\.exe[\s\S]*?Fault offset: 0x([0-9a-fA-F]+)') { $faultRva = $Matches[1] }
} else {
    Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue
}
if ($pm -and -not $pm.HasExited) { $pm.Kill() }
logman stop PresentMon -ets 2>&1 | Out-Null

[ordered]@{
    Name = $Name; Mode = $Mode; Seed = $Seed; BootSeconds = $BootSeconds; Minutes = $Minutes
    CaptureEverySec = $CaptureEverySec; HangSeconds = $HangSeconds; Env = $Env; ExtraArgs = $ExtraArgs
    Started = $started.ToString('s'); RanSeconds = $ran; Outcome = $outcome; ExitCode = $exitCode; FaultRva = $faultRva
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $session 'session.json')

python -I (Join-Path $PSScriptRoot 'autoplay-report.py') $session $map
