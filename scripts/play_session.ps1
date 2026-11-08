# Plays Shinobi stage 1 with a varied scripted pad (stick runs, circles, jumps, dashes, combos, kunai, guard), takes a
# screenshot every 2 s, then closes the game and prints the real frame rate, EE/VIF1 waits, hitch stacks and GS flush stats.
# usage: play_session.ps1 [-Seconds 170] [-Name play1] [-Seed 7] [-Env @{K='V'}] [-KeepShots]
param([int]$Seconds = 170, [string]$Name = 'play1', [int]$Seed = 7, [hashtable]$Env = @{}, [switch]$KeepShots)
$root = 'X:\RECOMPS - THE REAL AFSN\shinobi_port'
$log = "$root\game\captures\$Name.log"
New-Item -ItemType Directory -Force "$root\game\captures" | Out-Null
$rnd = New-Object System.Random $Seed
$items = New-Object System.Collections.Generic.List[string]
function Add($from, $len, $names) { $items.Add("$from-$($from + $len):$($names -join '+')") }
# intro: CROSS taps through the memory card dialogs, START taps through logos and cutscenes (never pressed in the stage)
foreach ($t in 2150, 2300) { Add $t 20 @('CROSS') }
for ($t = 2600; $t -le 9000; $t += 150) { Add $t 20 @('START') }
$dirs = @(@('LSUP'), @('LSDOWN'), @('LSLEFT'), @('LSRIGHT'), @('LSUP', 'LSLEFT'), @('LSUP', 'LSRIGHT'), @('LSDOWN', 'LSLEFT'), @('LSDOWN', 'LSRIGHT'))
$t = 3700
while ($t -lt 9400) {
    $d = $dirs[$rnd.Next($dirs.Count)]
    switch ($rnd.Next(15)) {
        0 { $len = 90 + $rnd.Next(120); Add $t $len $d; for ($k = 40; $k -lt $len; $k += 55) { Add ($t + $k) 6 (@('R1') + $d) }; $t += $len + 10 }          # run with dash taps
        1 { foreach ($c in @('LSUP'), @('LSRIGHT'), @('LSDOWN'), @('LSLEFT')) { Add $t 34 $c; $t += 34 }; $t += 6 }                                         # circle
        2 { Add $t 10 (@('CROSS') + $d); Add ($t + 12) 40 $d; Add ($t + 30) 8 @('SQUARE'); $t += 70 }                                                     # jump + air attack
        3 { foreach ($k in 0, 16, 32, 58) { Add ($t + $k) 7 (@('SQUARE') + $d) }; $t += 90 }                                                              # attack combo
        4 { foreach ($k in 0, 22, 44) { Add ($t + $k) 8 @('CIRCLE') }; $t += 70 }                                                                          # kunai
        5 { Add $t 45 (@('L1') + $d); $t += 55 }                                                                                                          # guard while moving
        6 { foreach ($k in 0, 18, 36, 54) { Add ($t + $k) 12 (@('R1') + $d) }; $t += 80 }                                                                  # dash chain
        7 { Add $t 10 @('TRIANGLE'); Add ($t + 14) 30 $d; $t += 50 }                                                                                      # special + step
        8 { $len = 70 + $rnd.Next(60); Add $t $len @($(@('UP','DOWN','LEFT','RIGHT')[$rnd.Next(4)])); if ($rnd.Next(2)) { Add ($t + 20) 6 @('R1') }; $t += $len + 8 }   # D-pad movement
        9 { $c = @($(@('RSLEFT','RSRIGHT','RSUP','RSDOWN')[$rnd.Next(4)])); Add $t 60 ($c + $d); $t += 70 }                                                 # camera stick while moving
        10 { Add $t 9 (@('L2') + $d); Add ($t + 25) 9 (@('R2') + $d); $t += 55 }                                                                              # L2 / R2
        11 { Add $t 7 @('L3'); Add ($t + 20) 7 @('R3'); $t += 40 }                                                                                             # stick clicks
        12 { Add $t 8 @('R1', 'SQUARE'); Add ($t + 20) 8 (@('L1', 'CIRCLE') + $d); Add ($t + 40) 8 @('CROSS', 'SQUARE'); $t += 65 }                              # two-button combos
        13 { $t += 30 + $rnd.Next(30) }                                                                                                                        # idle: stand still
        14 { Add $t 40 (@('R1', 'CROSS') + $d); Add ($t + 30) 12 @('TRIANGLE', 'SQUARE'); $t += 60 }                                                          # dash-jump into special
    }
}
$env:PS2X_PAD_SCRIPT = ($items -join ';')
$env:PS2X_PAD_SCRIPT_IN_STAGE = '1'
$env:PS2X_RENDERER = 'software'
$env:PS2X_STATS_LOG = '1'; $env:PS2X_VU1_STATS = '1'; $env:PS2X_GS_STATS = '1'; $env:PS2X_HITCH_LOG = '1'
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
$shots = "$root\play_shots_$Name"
New-Item -ItemType Directory -Force $shots | Out-Null
$cap = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList '-NoProfile', '-File', "`"$root\scripts\live_capture.ps1`"", '-Dir', "`"$shots`"", '-Every', '2'
Set-Location "$root\game"
$p = Start-Process -FilePath "$root\devilkings-fork\out\build\ps2xRuntime\ps2EntryRunner.exe" -ArgumentList "`"$root\game\SLUS_204.59`"" -WorkingDirectory "$root\game" -RedirectStandardOutput "$root\game\captures\$Name.out" -RedirectStandardError $log -PassThru
Start-Sleep $Seconds
if (-not $p.HasExited) { Stop-Process $p.Id -Force }
Start-Sleep 2
if (-not $cap.HasExited) { Stop-Process $cap.Id -Force }
Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item Env:PS2X_PAD_SCRIPT, Env:PS2X_PAD_SCRIPT_IN_STAGE, Env:PS2X_RENDERER, Env:PS2X_STATS_LOG, Env:PS2X_VU1_STATS, Env:PS2X_GS_STATS, Env:PS2X_HITCH_LOG -ErrorAction SilentlyContinue
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
"screenshots: $((Get-ChildItem $shots | Measure-Object).Count) in $shots"
python "$env:TEMP\ab.py" $log
