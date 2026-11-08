# Standard Shinobi test run with full telemetry. usage: run_test.ps1 [-Seconds 150] [-Name test1] [-Verify] [-Renderer software|vulkan] [-Threads auto|0] [-Record]
# Scripted input: NEW GAME (CROSS), START taps through the intro cutscenes (the pad script stops sending START by
# itself once the stage's VU1 load is detected, so it can never pause the stage), then SQUARE/UP/RIGHT in the stage.
param([int]$Seconds = 150, [string]$Name = 'test', [switch]$Verify, [string]$Renderer = 'software', [string]$Threads = '', [switch]$Record, [switch]$GameplayInput, [switch]$Profile, [switch]$GsAsync, [int]$FrameDumpFrom = 0, [string]$FrameDumpDir = '', [int]$FrameDumpDelay = 0)
$root = 'X:\RECOMPS - THE REAL AFSN\shinobi_port'
$log = "$root\game\captures\$Name.log"
New-Item -ItemType Directory -Force "$root\game\captures" | Out-Null
$a = '2150-2170:CROSS;2300-2320:CROSS'
$s = (2600..9000 | ? { $_ % 150 -eq 0 } | % { "$_-$($_+20):START" }) -join ';'
$c = (3600..40000 | ? { $_ % 90 -eq 0 } | % { "$_-$($_+25):SQUARE" }) -join ';'
$m = (3600..40000 | ? { $_ % 400 -eq 0 } | % { "$_-$($_+200):UP" }) -join ';'
$env:PS2X_PAD_SCRIPT = "$a;$s;$c;$m"
$env:PS2X_RENDERER = $Renderer
$env:PS2X_STATS_LOG = '1'; $env:PS2X_VU1_STATS = '1'; $env:PS2X_GS_STATS = '1'; $env:PS2X_HITCH_LOG = '1'
# By default the script stops pressing buttons once the stage starts, so you can play in the test window.
if ($GameplayInput) { $env:PS2X_PAD_SCRIPT_IN_STAGE = '1' }
if ($FrameDumpFrom -gt 0) { New-Item -ItemType Directory -Force $FrameDumpDir | Out-Null; $env:PS2X_FRAME_DUMP_DIR = $FrameDumpDir; $env:PS2X_FRAME_DUMP_INTERVAL = '1'; $env:PS2X_FRAME_DUMP_FROM = "$FrameDumpFrom"; $env:PS2X_FRAME_DUMP_STAGE = '1'; $env:PS2X_FRAME_DUMP_RAW = '1'; $env:PS2X_FRAME_DUMP_MAX = '240'; $env:PS2X_FRAME_DUMP_DELAY = "$FrameDumpDelay" }
if ($GsAsync) { $env:PS2X_GS_ASYNC = '1' }
if ($Profile) { $env:PS2X_PROFILE = '1' }
if ($Verify) { $env:PS2X_VU1_VERIFY = '1' }
if ($Threads -ne '') { $env:PS2X_VU1_THREADS = $Threads }
if ($Record) { $d = "$root\vu1_runs"; Remove-Item $d -Recurse -ErrorAction SilentlyContinue; New-Item -ItemType Directory $d | Out-Null
    $env:PS2X_VU1_RECORD = $d; $env:PS2X_VU1_RECORD_EVERY = '300'; $env:PS2X_VU1_RECORD_MAX = '3000'; $env:PS2X_VU1_THREADS = '0' }
Set-Location "$root\game"
$p = Start-Process -FilePath "$root\devilkings-fork\out\build\ps2xRuntime\ps2EntryRunner.exe" -ArgumentList "`"$root\game\SLUS_204.59`"" -WorkingDirectory "$root\game" -RedirectStandardOutput "$root\game\captures\$Name.out" -RedirectStandardError $log -PassThru
Start-Sleep $Seconds
if (-not $p.HasExited) { Stop-Process $p.Id -Force }
Remove-Item Env:PS2X_GS_STATS, Env:PS2X_HITCH_LOG, Env:PS2X_PAD_SCRIPT, Env:PS2X_RENDERER, Env:PS2X_STATS_LOG, Env:PS2X_VU1_STATS, Env:PS2X_VU1_VERIFY, Env:PS2X_VU1_THREADS, Env:PS2X_VU1_RECORD, Env:PS2X_VU1_RECORD_EVERY, Env:PS2X_VU1_RECORD_MAX, Env:PS2X_PAD_SCRIPT_IN_STAGE, Env:PS2X_PROFILE, Env:PS2X_GS_ASYNC, Env:PS2X_FRAME_DUMP_DIR, Env:PS2X_FRAME_DUMP_INTERVAL, Env:PS2X_FRAME_DUMP_FROM, Env:PS2X_FRAME_DUMP_STAGE, Env:PS2X_FRAME_DUMP_RAW, Env:PS2X_FRAME_DUMP_MAX -ErrorAction SilentlyContinue
python "$root\scripts\analyze_run.py" $log
