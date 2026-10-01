$ErrorActionPreference = "Stop"
$TaskName = "GIGA Dashboard Bridge"
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
  Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
  Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
  Write-Host "Autostart removed."
} else {
  Write-Host "No autostart task found."
}
