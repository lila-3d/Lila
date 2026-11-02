# Registers (or with -Remove, deletes) the LilaNightly scheduled task: scripts\nightly.py at 04:00 daily, as the
# current user, no console window, catching up after the machine was asleep.
param([switch]$Remove, [string]$At = "04:00")

$name = "LilaNightly"
if ($Remove) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false
    return
}
$repo = Split-Path -Parent $PSScriptRoot
$pythonw = (Get-Command pythonw.exe -ErrorAction Stop).Source
$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$repo\scripts\nightly.py`"" -WorkingDirectory $repo
$trigger = New-ScheduledTaskTrigger -Daily -At $At
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -DontStopIfGoingOnBatteries -AllowStartIfOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 6)
Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
"Registered $name at $At daily: $pythonw $repo\scripts\nightly.py"
