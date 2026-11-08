@echo off
set "JAVA_HOME=X:\HAPPYNESS\jdk25\jdk-25.0.5+8"
set "G=X:\HAPPYNESS\ghidra_12.2_PUBLIC_20261016\ghidra_12.2_PUBLIC"
call "%G%\support\analyzeHeadless.bat" "X:\HAPPYNESS\shinobi_ghidra" Shinobi -process SLUS_204.59 -noanalysis -scriptPath "%~dp0." -postScript ExportPS2Functions.java
