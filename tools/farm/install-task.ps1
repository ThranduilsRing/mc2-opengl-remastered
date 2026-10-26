param([string]$Python = (python -c 'import sys; print(sys.executable)'))
$ErrorActionPreference = 'Stop'
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$action = New-ScheduledTaskAction -Execute $Python -Argument 'C:\mc2farm\runner.py --root C:\mc2farm' -WorkingDirectory 'C:\mc2farm'
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$settings.Priority = 7 # BELOW_NORMAL, inherited by harness children.
Register-ScheduledTask -TaskName 'MC2FarmRunner' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'MC2FarmRunner'
Get-ScheduledTask -TaskName 'MC2FarmRunner'
