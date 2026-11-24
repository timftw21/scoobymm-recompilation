[CmdletBinding()]
param(
    [ValidateSet('Menu','Library','Walk','Ghost','Movie')][string]$Scene = 'Library',
    [ValidateRange(0,1000)][int]$RefreshRate = 360,
    [ValidateRange(30,3000)][int]$Frames = 240,
    [string]$Game,
    [string]$Output,
    # Rendering enhancements; the defaults measure the original image.
    [ValidateRange(1,6)][int]$RenderScale = 1,
    [ValidateSet(1,2,4,8)][int]$Msaa = 1,
    [switch]$Fxaa,
    [ValidateSet(1,2,4,8,16)][int]$Anisotropy = 1
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
if (!$Game) {
    $Game = Join-Path $root 'build/xbe-release/scoobymm.exe'
    if (!(Test-Path -LiteralPath $Game)) { $Game = Join-Path $root 'build/xbe-compiled/scoobymm.exe' }
}
if (!$Output) { $Output = Join-Path $root ("build/bench-{0}-{1:yyyyMMdd-HHmmss-fff}" -f $Scene,(Get-Date)) }
$run = (New-Item -ItemType Directory -Path $Output).FullName
New-Item -ItemType Directory -Path "$run/profile" | Out-Null
Set-Content -LiteralPath "$run/profile/settings.cfg" -Encoding ascii -Value @(
    "render_scale=$RenderScale", "msaa=$Msaa", "fxaa=$([int][bool]$Fxaa)", "anisotropy=$Anisotropy")
# Navigate at the original cadence so the same input reaches the same scene at every cap.
$options = @('--hidden','--mute','--no-vsync','--widescreen','--refresh-rate','30',
             '--storage',"$run/profile",'--report',"$run/session.json")
if ($Scene -ne 'Movie') {
    $last = if ($Scene -eq 'Menu') { 12 } else { 22 }
    foreach ($n in 1..$last) { $options += '--start-at-scanout', [string]($n*10) }
}
switch ($Scene) {
    # A 100-frame sample starting at scanout 100 stays inside WBPH.XMV,
    # where consecutive pictures are 33 or 34 milliseconds apart.
    'Movie' { $start = 70 }
    'Menu' {
        $options += '--start-at-scanout', '720'
        $start = 800
    }
    { $_ -in 'Library','Walk','Ghost' } {
        foreach ($n in 1..20) { $options += '--confirm-at-scanout', [string](250+$n*34) }
        $start = 1100
        if ($Scene -eq 'Ghost') {
            # Walk through the double doors, dismiss the save tutorial and approach the blue ghost.
            $options += '--stick-at-scanout', '1130', '55', '0', '32767',
                        '--stick-at-scanout', '1185', '13', '28000', '13000',
                        '--stick-at-scanout', '1198', '58', '0', '32767',
                        '--confirm-at-scanout', '1256', '--confirm-at-scanout', '1456',
                        '--stick-at-scanout', '1511', '65', '0', '-32767',
                        '--confirm-at-scanout', '1576',
                        '--stick-at-scanout', '1650', '23', '32767', '0',
                        '--stick-at-scanout', '1673', '45', '-3600', '32500',
                        '--confirm-at-scanout', '1868',
                        '--stick-at-scanout', '1943', '35', '2500', '32600'
            $start = 1978
        }
    }
}
$measure = $start + 30
if ($Scene -eq 'Walk') {
    $options += '--stick-at-scanout', [string]$measure, [string]$Frames, '32767', '0'
}
$options += '--refresh-at-scanout', [string]$start, [string]$RefreshRate,
            '--performance', "$run/performance", '--performance-at', [string]$measure,
            '--performance-frames', [string]$Frames, '--frame', "$run/scene.png",
            '--frame-index', [string]($measure+$Frames+1), '--frames', [string]($measure+$Frames+2)
# Piping the output waits for the windowed program and keeps its exit code.
& $Game @options | Out-Host
if ($LASTEXITCODE) { throw "Game stopped with exit code $LASTEXITCODE. See $run/session.json" }
$stats = Get-Content -Raw "$run/performance/performance.json" | ConvertFrom-Json
if ($stats.scanouts -ne $Frames) { throw "Measurement ended early. See $run/performance" }
if ($Scene -ne 'Movie' -and ($stats.overlay_scanouts -or $stats.guest_flips -ne $Frames -or !$stats.engine_updates)) {
    throw "The sample left the rendered game scene. Check $run/scene.png and performance.csv."
}
if ($Scene -eq 'Movie' -and ($stats.overlay_scanouts -lt [Math]::Min(100,$Frames) -or $stats.presentations -lt $stats.overlay_scanouts)) {
    throw "The movie sample missed overlays or dropped presentations. Check $run/performance."
}
if ($Scene -ne 'Movie') {
    $session = Get-Content -Raw "$run/session.json" | ConvertFrom-Json
    $sceneModule = switch ($Scene) {
        'Menu'  { 'title_camera' }
        'Ghost' { 'medghostlevelone' }
        default { 'coll_level1' }
    }
    if ($sceneModule -notin $session.logic_modules.name) { throw "Expected $sceneModule in the captured scene. Check $run/scene.png." }
}
$stats | Select-Object processor,graphics_adapter,native_size,render_size,msaa,fxaa,anisotropy,window_size,refresh_rate,measured_scanouts_per_second,presentations_per_second,
    guest_flips,presentations,engine_updates,simulation_ms,measured_ms,frame,game_thread_cpu,game_thread_cycles,submission,gpu_wait,gpu_command_span
Write-Output "Results in $run"
