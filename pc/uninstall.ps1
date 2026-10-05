#Requires -RunAsAdministrator
# Removes everything install.ps1 created
Get-Process hadal-tray -ErrorAction SilentlyContinue | Stop-Process -Force
if (Get-Service hadal -ErrorAction SilentlyContinue) { Stop-Service hadal -ErrorAction SilentlyContinue; sc.exe delete hadal | Out-Null }
Get-NetFirewallRule -DisplayName Hadal -ErrorAction SilentlyContinue | Remove-NetFirewallRule
Remove-Item "$([Environment]::GetFolderPath('Startup'))\hadal-tray.lnk" -ErrorAction SilentlyContinue
Start-Sleep 1 # Wait for the service to exit
Remove-Item "$env:ProgramFiles\Hadal", "$env:ProgramData\Hadal" -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "HKCU:\Software\Hadal" -Recurse -ErrorAction SilentlyContinue
Write-Host "Hadal removed."
