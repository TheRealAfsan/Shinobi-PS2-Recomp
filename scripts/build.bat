@echo off
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "T=X:\RECOMPS - THE REAL AFSN\shinobi_port\tools"
set "PATH=%T%\cmake-4.4.5-windows-x86_64\bin;%T%\ninja;%PATH%"
cd /d X:\RECOMPS - THE REAL AFSN\shinobi_port\devilkings-fork
cmake --build out\build --target %*
