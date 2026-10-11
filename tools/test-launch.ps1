param(
    [Parameter(Mandatory=$true)][int]$Attempt,
    [int]$MonitorSeconds = 15,
    [hashtable]$GpuOverrides = @{},
    [switch]$DrawDiagnostics
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $projectRoot 'out\build\local-win-release\BullyRecomp.exe'
$gameRoot = Join-Path $projectRoot 'assets\game'
$logPath = Join-Path $projectRoot "logs\runtime-$Attempt.log"
$existingRun = Get-CimInstance Win32_Process -Filter "Name='BullyRecomp.exe'" |
    Where-Object { $_.ExecutablePath -eq $exePath }
if ($existingRun) { throw 'The project executable is already running; inspect that attempt first.' }
if (Test-Path -LiteralPath $logPath) { throw "Attempt log already exists: $logPath" }
$launchArgs = @('--game_data_root', ('"' + $gameRoot + '"'),
    '--log_file', ('"' + $logPath + '"'), '--log_level', 'debug',
    '--allow_game_relative_writes', '--mnk_mode', '--bully_capture_dir',
    ('"' + (Join-Path $projectRoot "logs\captures\attempt-$Attempt") + '"'))
if ($DrawDiagnostics) { $launchArgs += '--bully_gpu_diagnostics' }
$previousEnvironment = @{}
try {
    foreach ($flagName in $GpuOverrides.Keys) {
        $envName = 'REX_' + $flagName.ToUpperInvariant()
        $previousEnvironment[$envName] = [Environment]::GetEnvironmentVariable($envName, 'Process')
        [Environment]::SetEnvironmentVariable($envName, [string]$GpuOverrides[$flagName], 'Process')
    }
    $launchRun = Start-Process -FilePath $exePath -ArgumentList $launchArgs `
        -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru
} finally {
    foreach ($envName in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($envName, $previousEnvironment[$envName], 'Process')
    }
}
$launchRun.Id | Set-Content (Join-Path $projectRoot 'logs\runtime-pid.txt')
$didExit = $launchRun.WaitForExit($MonitorSeconds * 1000)
$launchRun.Refresh()
$result = [ordered]@{Attempt=$Attempt;PID=$launchRun.Id;Exited=$didExit;Log=$logPath}
$result.GpuOverrides = $GpuOverrides
if ($didExit) { $result.ExitCode=$launchRun.ExitCode }
else { $result.CPUSeconds=$launchRun.TotalProcessorTime.TotalSeconds }
$result.LastFatal = (Select-String -LiteralPath $logPath -Pattern '\[FATAL\]' |
    Select-Object -Last 1 -ExpandProperty Line)
$result | ConvertTo-Json | Tee-Object -FilePath (Join-Path $projectRoot "logs\runtime-$Attempt-result.json")
Get-Content -LiteralPath $logPath -Tail 25
