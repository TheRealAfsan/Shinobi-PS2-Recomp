@echo off
rem Waits for the Shinobi game build (ninja.exe) to finish, records the result, then shuts the PC down.
set "LOG=X:\RECOMPS - THE REAL AFSN\shinobi_port\shutdown_watch.log"
echo %date% %time% watcher started > "%LOG%"
:wait
tasklist /FI "IMAGENAME eq ninja.exe" 2>nul | find /I "ninja.exe" >nul
if not errorlevel 1 (
    timeout /t 30 /nobreak >nul
    goto wait
)
echo %date% %time% build process ended >> "%LOG%"
if exist "X:\RECOMPS - THE REAL AFSN\shinobi_port\devilkings-fork\out\build\ps2xRuntime\ps2EntryRunner.exe" (
    for %%F in ("X:\RECOMPS - THE REAL AFSN\shinobi_port\devilkings-fork\out\build\ps2xRuntime\ps2EntryRunner.exe") do echo exe size: %%~zF >> "%LOG%"
)
echo shutting down in 120s - cancel with: shutdown /a >> "%LOG%"
shutdown /s /t 120 /c "Shinobi build finished. Shutting down in 2 minutes. Cancel with: shutdown /a"
