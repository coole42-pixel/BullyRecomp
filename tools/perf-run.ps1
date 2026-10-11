param(
    [Parameter(Mandatory=$true)][string]$Name,
    [int]$LoadSeconds = 50,      # time for the game to load into the world before measuring
    [int]$MeasureSeconds = 45,   # measurement window (scripted walk-around)
    [hashtable]$Env = @{},
    [string[]]$ExtraArgs = @(),
    [string]$LogLevel = 'warning',
    [switch]$Profile,            # also sample the guest main thread and resolve hot functions
    [switch]$NoInput             # skip the scripted walk (Jimmy stands still)
)
# Fully automatic benchmark: launches the game in a visible window, waits for the world,
# drives Jimmy with injected keyboard input (W + camera arrows), records presents with
# PresentMon plus the in-process guest-swap/CPU log, kills the game, and writes
# logs/perf/<Name>.summary.json. A visible window is required: a minimized window presents nothing.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'out\build\local-win-release\BullyRecomp.exe'
$presentMon = Join-Path $root 'tools\presentmon\PresentMon.exe'
$perfDir = Join-Path $root 'logs\perf'
New-Item -ItemType Directory -Force $perfDir | Out-Null
$csv = Join-Path $perfDir "$Name.csv"
$pmCsv = Join-Path $perfDir "$Name.presentmon.csv"
Remove-Item $csv, $pmCsv -ErrorAction SilentlyContinue
Get-Process BullyRecomp, PresentMon -ErrorAction SilentlyContinue | Stop-Process -Force
# A killed PresentMon leaves its ETW session running, and later captures then record nothing.
logman stop PresentMon -ets 2>&1 | Out-Null
Start-Sleep -Seconds 2

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class BullyInput {
  [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)] struct U { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public U u; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public static void Key(ushort scan, bool down, bool extended) {
    var i = new INPUT[1]; i[0].type = 1;
    i[0].u.ki.wScan = scan;
    i[0].u.ki.dwFlags = 0x8 | (down ? 0u : 0x2u) | (extended ? 0x1u : 0u);
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static void Focus(IntPtr h) {
    keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero); // unlock foreground
    ShowWindow(h, 9); SetForegroundWindow(h);
  }
}
'@

$launch = @('--game_data_root', ('"' + (Join-Path $root 'assets\game') + '"'),
    '--log_file', ('"' + (Join-Path $perfDir "$Name.log") + '"'), '--log_level', $LogLevel,
    '--allow_game_relative_writes', '--mnk_mode', '--bully_ini', 'none', '--bully_perf_log', ('"' + $csv + '"')) + $ExtraArgs
if ($Profile) { $launch += '--bully_profile' }
$saved = @{}
foreach ($k in $Env.Keys) {
    $n = 'REX_' + $k.ToUpperInvariant()
    $saved[$n] = [Environment]::GetEnvironmentVariable($n, 'Process')
    [Environment]::SetEnvironmentVariable($n, [string]$Env[$k], 'Process')
}
try {
    $game = Start-Process -FilePath $exe -ArgumentList $launch -WorkingDirectory $root -WindowStyle Normal -PassThru
} finally {
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n], 'Process') }
}
$pm = Start-Process -FilePath $presentMon -WindowStyle Hidden -PassThru -ArgumentList @(
    '--process_id', $game.Id, '--output_file', ('"' + $pmCsv + '"'), '--delay', $LoadSeconds,
    '--timed', $MeasureSeconds, '--terminate_after_timed', '--no_console_stats')

$crashed = $false
$deadline = (Get-Date).AddSeconds($LoadSeconds)
while ((Get-Date) -lt $deadline) { if ($game.HasExited) { $crashed = $true; break }; Start-Sleep -Milliseconds 500 }
if (-not $crashed) {
    $game.Refresh()
    [BullyInput]::Focus($game.MainWindowHandle)
    Start-Sleep -Milliseconds 500
    $end = (Get-Date).AddSeconds($MeasureSeconds)
    if (-not $NoInput) { [BullyInput]::Key(0x11, $true, $false) }   # hold W (run forward)
    $turn = 0
    while ((Get-Date) -lt $end) {
        if ($game.HasExited) { $crashed = $true; break }
        if (-not $NoInput) {
            # sweep the camera: right, then left, using the arrow keys (right stick)
            $scan = if ($turn % 2 -eq 0) { 0x4D } else { 0x4B }
            [BullyInput]::Key($scan, $true, $true); Start-Sleep -Milliseconds 900
            [BullyInput]::Key($scan, $false, $true)
            $turn++
        }
        Start-Sleep -Milliseconds 2500
    }
    if (-not $NoInput) { [BullyInput]::Key(0x11, $false, $false) }
}
$pm.WaitForExit(15000) | Out-Null
if (-not $pm.HasExited) { $pm.Kill() }
$exitCode = $null
if ($game.HasExited) { $exitCode = $game.ExitCode } else { $game.Kill(); $game.WaitForExit() }

function Pct($v, $q) { if ($v.Count) { $s = @($v | Sort-Object); $s[[math]::Min($s.Count-1, [int][math]::Floor($q*$s.Count))] } else { $null } }
function Avg($v) { if ($v.Count) { [math]::Round(($v | Measure-Object -Average).Average, 3) } else { $null } }
$pmRows = @(); if (Test-Path $pmCsv) { $pmRows = @(Import-Csv $pmCsv) }
$ft = @($pmRows | ForEach-Object { [double]$_.MsBetweenPresents })
$perfRows = @(); if (Test-Path $csv) { $perfRows = @(Import-Csv $csv) }
$steadyStart = $LoadSeconds; $steady = @($perfRows | Where-Object { [double]$_.t_s -ge $steadyStart })
$hitch = @($ft | Where-Object { $_ -gt 33.4 }).Count
# Exact per-swap intervals (BullyFrameSwapHook QPC ring) for frame-pacing metrics.
$gf = @(); if (Test-Path "$csv.frames.csv") {
    $gf = @(Import-Csv "$csv.frames.csv" | Where-Object { [double]$_.t_s -ge $steadyStart } | ForEach-Object { [double]$_.frame_ms })
}
$gfJitter = $null
if ($gf.Count -gt 1) { $sum = 0.0; for ($i = 1; $i -lt $gf.Count; $i++) { $sum += [math]::Abs($gf[$i] - $gf[$i-1]) }; $gfJitter = [math]::Round($sum / ($gf.Count - 1), 3) }
$gfStd = $null
if ($gf.Count) { $m = (Avg $gf); $gfStd = [math]::Round([math]::Sqrt((($gf | ForEach-Object { ($_ - $m) * ($_ - $m) }) | Measure-Object -Sum).Sum / $gf.Count), 3) }
$summary = [ordered]@{
    Name = $Name; Env = $Env; ExtraArgs = $ExtraArgs; Input = (-not $NoInput)
    Crashed = $crashed; ExitCode = $exitCode
    Presents = $ft.Count
    AvgFps = if ($ft.Count) { [math]::Round(1000.0 / (Avg $ft), 2) } else { $null }
    P50FrameMs = Pct $ft 0.5
    P99FrameMs = Pct $ft 0.99
    P999FrameMs = Pct $ft 0.999
    Low1PctFps = if ($ft.Count) { [math]::Round(1000.0 / (Pct $ft 0.99), 2) } else { $null }
    WorstFrameMs = if ($ft.Count) { ($ft | Measure-Object -Maximum).Maximum } else { $null }
    FramesOver33ms = $hitch
    FramesOver20ms = @($ft | Where-Object { $_ -gt 20 }).Count
    GpuBusyMs = Avg @($pmRows | ForEach-Object { [double]$_.MsGPUBusy })
    CpuBusyMs = Avg @($pmRows | ForEach-Object { [double]$_.MsCPUBusy })
    GuestSwapsPerSec = Avg @($steady | ForEach-Object { [double]$_.fps })
    GuestAvgMs = Avg @($steady | ForEach-Object { [double]$_.avg_ms })
    GuestP99Ms = Avg @($steady | ForEach-Object { [double]$_.p99_ms })
    GuestWorstMs = if ($steady.Count) { ($steady | ForEach-Object { [double]$_.max_ms } | Measure-Object -Maximum).Maximum } else { $null }
    GuestFrames = $gf.Count
    GuestFrameP50Ms = Pct $gf 0.5
    GuestFrameP95Ms = Pct $gf 0.95
    GuestFrameP99Ms = Pct $gf 0.99
    GuestFrameStdMs = $gfStd
    GuestFrameJitterMs = $gfJitter   # mean |difference| between consecutive frame times
    GuestFramesOver20ms = @($gf | Where-Object { $_ -gt 20 }).Count
    GuestFramesUnder13ms = @($gf | Where-Object { $_ -lt 13 }).Count
    DisplayChangeStdMs = $(
        $dc = @($pmRows | Where-Object { $_.MsBetweenDisplayChange -ne 'NA' } | ForEach-Object { [double]$_.MsBetweenDisplayChange })
        if ($dc.Count) { $m = Avg $dc; [math]::Round([math]::Sqrt((($dc | ForEach-Object { ($_ - $m) * ($_ - $m) }) | Measure-Object -Sum).Sum / $dc.Count), 3) } else { $null })
    ProcessCpuCores = Avg @($steady | ForEach-Object { [double]$_.cpu_cores })
    WorkingSetMb = if ($steady.Count) { ($steady | ForEach-Object { [double]$_.ws_mb } | Measure-Object -Maximum).Maximum } else { $null }
}
if ($Profile -and (Test-Path "$csv.profile.csv")) {
    python -I (Join-Path $PSScriptRoot 'resolve-profile.py') (Join-Path $root 'out\build\local-win-release\BullyRecomp.map') "$csv.profile.csv" 40 |
        Set-Content (Join-Path $perfDir "$Name.profile.txt")
}
$summary | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $perfDir "$Name.summary.json")
$summary | ConvertTo-Json -Depth 4 -Compress
