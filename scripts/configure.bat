@echo off
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set "T=X:\RECOMPS - THE REAL AFSN\shinobi_port\tools"
set "PATH=%T%\cmake-4.4.5-windows-x86_64\bin;%T%\ninja;%PATH%"
cd /d X:\RECOMPS - THE REAL AFSN\shinobi_port\devilkings-fork
rem Incremental reconfigure of the existing build tree (extra -D flags are passed as arguments).
cmake -S . -B out\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_PGS_DIR=X:/HAPPYNESS/shinobi_port/parallel-gs -DPS2X_ENABLE_RUNTIME_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF %*
