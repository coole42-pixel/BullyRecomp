# Runs a set of configurations through perf-run.ps1 back to back and writes logs/perf/matrix.md.
# Fully automatic; takes roughly 2 minutes per configuration. Window focus is taken while it runs.
param([string]$Out = 'matrix', [string[]]$Only = @(), [int]$Set = 1)
$ErrorActionPreference = 'Stop'
$configs = [ordered]@{
    'baseline'              = @{}
    'present-nonui'         = @{ host_present_from_non_ui_thread = 'true' }
    'vsync-off'             = @{ vsync = 'false' }
    'present-nonui+vsync-off' = @{ host_present_from_non_ui_thread = 'true'; vsync = 'false' }
    'submit-primary-end'    = @{ d3d12_submit_on_primary_buffer_end = 'false' }
    'async-shaders'         = @{ async_shader_compilation = 'true' }
}
if ($Set -eq 2) {
    # builds on the matrix1 winner (submit_on_primary_buffer_end=false)
    $s = @{ d3d12_submit_on_primary_buffer_end = 'false' }
    $configs = [ordered]@{
        'submit-false'              = $s
        'submit-false+vsync-off'    = $s + @{ vsync = 'false' }
        'submit-false+nonui+vsync-off' = $s + @{ host_present_from_non_ui_thread = 'true'; vsync = 'false' }
        'submit-false+async'        = $s + @{ async_shader_compilation = 'true' }
        'submit-false+pipeline-threads' = $s + @{ d3d12_pipeline_creation_threads = '8' }
        'submit-false+tiled-sm'     = $s + @{ d3d12_tiled_shared_memory = 'false' }
    }
}
$rows = @()
foreach ($name in $configs.Keys) {
    if ($Only.Count -and $Only -notcontains $name) { continue }
    $json = & (Join-Path $PSScriptRoot 'perf-run.ps1') -Name "$Out-$name" -Env $configs[$name] | Select-Object -Last 1
    $rows += ($json | ConvertFrom-Json) | Add-Member -NotePropertyName Config -NotePropertyValue $name -PassThru
}
$md = @('| Config | Guest swaps/s | Guest avg ms | Guest p99 ms | Guest worst ms | Display fps | Display 1% low | Hitches >33ms | Crashed |', '|---|---|---|---|---|---|---|---|---|')
foreach ($r in $rows) { $md += "| $($r.Config) | $($r.GuestSwapsPerSec) | $($r.GuestAvgMs) | $($r.GuestP99Ms) | $($r.GuestWorstMs) | $($r.AvgFps) | $($r.Low1PctFps) | $($r.FramesOver33ms) | $($r.Crashed) |" }
$md | Set-Content (Join-Path $PSScriptRoot "..\logs\perf\$Out.md")
$md
