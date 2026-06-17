param(
    [Parameter(Mandatory=$true)][string[]]$Specs
)
$ErrorActionPreference = "Stop"
$dst = "C:\ProgramData\VoidDrivers\tools"
New-Item -ItemType Directory -Path $dst -Force | Out-Null
$dstExe = Join-Path $dst "SetVoidTopology.exe"
Copy-Item -Path "C:\Windows\Temp\SetVoidTopology.exe" -Destination $dstExe -Force
"Copied to $dstExe"

# Build a per-display args string for the scheduled task from the spec list
$argStr = ($Specs | ForEach-Object { "`"$_`"" }) -join " "
$taskName = "VoidDisplayTopology"

# Remove any existing task
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

# Create a per-user-at-logon task. Run level 1 = highest; principal = current user.
$action = New-ScheduledTaskAction -Execute $dstExe -Argument $argStr -WorkingDirectory $dst
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
"Registered scheduled task: $taskName"
"  Trigger : AtLogOn"
"  User    : $env:USERNAME"
"  Action  : $dstExe $argStr"

# Run it now in the current user session
"=== running now ==="
& $dstExe @Specs
"Exit: $LASTEXITCODE"
