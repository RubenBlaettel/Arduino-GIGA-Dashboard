<#
.SYNOPSIS
  Start the GIGA dashboard bridge at logon (Windows Task Scheduler, current user, no window).
  Remove again with uninstall_autostart.ps1.
#>
$ErrorActionPreference = "Stop"
$TaskName = "GIGA Dashboard Bridge"
$Here = $PSScriptRoot

$pyw = Get-Command pythonw.exe -ErrorAction SilentlyContinue
if (-not $pyw) { throw "pythonw.exe not found on PATH. Install Python 3.11+ from python.org." }

$action    = New-ScheduledTaskAction -Execute $pyw.Source -Argument "`"$Here\bridge.py`"" -WorkingDirectory $Here
$trigger   = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
               -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) `
               -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
  -Principal $principal -Description "Sends PC time and HWiNFO temperatures to the Arduino GIGA dashboard." -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Host "Autostart installed and bridge started ($TaskName)."
