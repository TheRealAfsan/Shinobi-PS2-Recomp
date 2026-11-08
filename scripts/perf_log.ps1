param([string]$Out, [string]$Proc='pcsx2-qt', [double]$Every=0.5)
$cores=[Environment]::ProcessorCount
$total=New-Object System.Diagnostics.PerformanceCounter('Processor','% Processor Time','_Total'); [void]$total.NextValue()
"t_s,proc_cpu_pct_of_machine,total_cpu_pct,ram_mb" | Set-Content $Out
$sw=[Diagnostics.Stopwatch]::StartNew(); $prev=$null; $prevT=0
while($true){
  $p=Get-Process $Proc -ErrorAction SilentlyContinue | Select -First 1
  if(-not $p){ if($prev -ne $null){ break } else { Start-Sleep -Milliseconds 500; continue } }
  $cpu=$p.TotalProcessorTime.TotalSeconds; $t=$sw.Elapsed.TotalSeconds
  if($prev -ne $null -and $t -gt $prevT){ $pc=100*($cpu-$prev)/(($t-$prevT)*$cores); ("{0:N1},{1:N1},{2:N1},{3}" -f $t,$pc,$total.NextValue(),[int]($p.WorkingSet64/1MB)) | Add-Content $Out }
  $prev=$cpu; $prevT=$t; Start-Sleep -Milliseconds ([int]($Every*1000))
}
