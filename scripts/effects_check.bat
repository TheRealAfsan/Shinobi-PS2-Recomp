@echo off
rem Effects check: replays a recorded drawing stream (game\captures\gs_stream.bin, recorded with the launcher's
rem "Record drawing (effects check)" mod) through four renderer setups, without running the game, then compares them:
rem   A  PCSX2 software renderer at the PS2's own resolution - the reference (PCSX2's most exact renderer)
rem   B  Direct3D 11 at 4x with Shinobi's settings (Ridge Racer fixes off, native scaling on)  - what the game uses now
rem   C  Direct3D 11 at 4x with the Ridge Racer fixes on and native scaling off                - how it was before
rem   D  Direct3D 11 at 4x, Ridge Racer fixes off, native scaling off                          - native scaling's own effect
rem Output: game\captures\effects_check\ (frames, side-by-side pictures, effects_report.txt).
rem usage: effects_check.bat [recording] [every-Nth-frame]
setlocal
set "ROOT=X:\RECOMPS - THE REAL AFSN\shinobi_port"
set "RUNNER=%ROOT%\devilkings-fork\out\build\ps2xRuntime\ps2EntryRunner.exe"
set "REC=%~1"
if "%REC%"=="" set "REC=%ROOT%\game\captures\gs_stream.bin"
set "EVERY=%~2"
if "%EVERY%"=="" set "EVERY=120"
set "OUT=%ROOT%\game\captures\effects_check"
if not exist "%REC%" (
  echo No recording at %REC%
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\*.rgba" 2>nul
cd /d "%OUT%"

rem The replays must not pick up the game's settings.ini.
set PS2X_SETTINGS=off
set PS2X_PCSX2_REPLAY=%REC%
set PS2X_PCSX2_REPLAY_EVERY=%EVERY%

echo [A] software, native ...
set PS2X_PCSX2_RENDERER=software
set PS2X_PCSX2_SCALE=1
set PS2X_PCSX2_REPLAY_OUT=%OUT%\A
"%RUNNER%" > "%OUT%\A_hashes.txt" 2> "%OUT%\A_log.txt"

set PS2X_PCSX2_RENDERER=d3d11
set PS2X_PCSX2_SCALE=4

echo [B] d3d11 4x, Shinobi settings ...
set RRV_PCSX2_GS_FULL_DOF_NATIVE=0
set RRV_PCSX2_GS_FULL_LARGE_ST=0
set RRV_PCSX2_GS_FULL_NATIVE_SCALING=1
set PS2X_PCSX2_REPLAY_OUT=%OUT%\B
"%RUNNER%" > "%OUT%\B_hashes.txt" 2> "%OUT%\B_log.txt"

echo [C] d3d11 4x, Ridge Racer fixes (as before) ...
set RRV_PCSX2_GS_FULL_DOF_NATIVE=1
set RRV_PCSX2_GS_FULL_LARGE_ST=2
set RRV_PCSX2_GS_FULL_NATIVE_SCALING=0
set PS2X_PCSX2_REPLAY_OUT=%OUT%\C
"%RUNNER%" > "%OUT%\C_hashes.txt" 2> "%OUT%\C_log.txt"

echo [D] d3d11 4x, fixes off, native scaling off ...
set RRV_PCSX2_GS_FULL_DOF_NATIVE=0
set RRV_PCSX2_GS_FULL_LARGE_ST=0
set RRV_PCSX2_GS_FULL_NATIVE_SCALING=0
set PS2X_PCSX2_REPLAY_OUT=%OUT%\D
"%RUNNER%" > "%OUT%\D_hashes.txt" 2> "%OUT%\D_log.txt"

echo [E] Vulkan 4x, Shinobi settings (PCSX2's Vulkan renderer has frame-buffer feedback that Direct3D 11 lacks) ...
set PS2X_PCSX2_RENDERER=vulkan
set RRV_PCSX2_GS_FULL_NATIVE_SCALING=1
set PS2X_PCSX2_REPLAY_OUT=%OUT%\E
"%RUNNER%" > "%OUT%\E_hashes.txt" 2> "%OUT%\E_log.txt"

echo Comparing ...
python "%ROOT%\scripts\effects_compare.py" "%OUT%" > "%OUT%\effects_report.txt"
python -I "%ROOT%\scripts\gs_stream_scan.py" "%REC%" --every 600 >> "%OUT%\effects_report.txt"
type "%OUT%\effects_report.txt"
endlocal
