#Requires -RunAsAdministrator
# Creates: Program Files\Hadal, ProgramData\Hadal, service, firewall rule, Startup shortcut
$ErrorActionPreference = 'Stop'
$src = if (Test-Path "$PSScriptRoot\hadal-svc.exe") { $PSScriptRoot } else { Join-Path $PSScriptRoot 'build\Release' }
$dst = "$env:ProgramFiles\Hadal"
$data = "$env:ProgramData\Hadal"
$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function sc_ { & sc.exe @args | Out-Null; if ($LASTEXITCODE) { throw "sc.exe $args failed ($LASTEXITCODE)" } }

foreach ($f in 'hadal-svc.exe', 'hadal-tray.exe') { if (-not (Test-Path "$src\$f")) { throw "$f not found next to install.ps1" } }

# Stop old install
Get-Process hadal-tray -ErrorAction SilentlyContinue | Stop-Process -Force
if (Get-Service hadal -ErrorAction SilentlyContinue) { Stop-Service hadal }

New-Item -ItemType Directory -Force $dst, $data | Out-Null
Copy-Item "$src\hadal-svc.exe", "$src\hadal-tray.exe", "$src\hadal-svc.pdb", "$src\hadal-tray.pdb" $dst -Force

if (-not (Get-Service hadal -ErrorAction SilentlyContinue)) {
    New-Service -Name hadal -BinaryPathName "`"$dst\hadal-svc.exe`" $sid" -DisplayName 'Hadal remote control' `
        -Description "Remote control from your phone over Tailscale. Uninstall: $dst\Uninstall.cmd" -StartupType Automatic | Out-Null
}
sc_ config hadal obj= 'NT AUTHORITY\LocalService'
sc_ sidtype hadal unrestricted # Own service SID
sc_ privs hadal SeShutdownPrivilege/SeChangeNotifyPrivilege # Only shutdown
sc_ failure hadal reset= 86400 actions= restart/5000/restart/5000/restart/60000

& icacls.exe $data /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' 'NT SERVICE\hadal:(OI)(CI)M' "*${sid}:(OI)(CI)RX" | Out-Null
if ($LASTEXITCODE) { throw "icacls failed" }

Get-NetFirewallRule -DisplayName Hadal -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName Hadal -Direction Inbound -Action Allow -Protocol TCP -LocalPort 47810 `
    -RemoteAddress 100.64.0.0/10 -Program "$dst\hadal-svc.exe" -Profile Any | Out-Null

$lnk = (New-Object -ComObject WScript.Shell).CreateShortcut("$([Environment]::GetFolderPath('Startup'))\hadal-tray.lnk")
$lnk.TargetPath = "$dst\hadal-tray.exe"
$lnk.Save()
Copy-Item "$PSScriptRoot\uninstall.ps1", "$PSScriptRoot\Uninstall.cmd" $dst -Force

Start-Service hadal
Start-Process explorer.exe "$dst\hadal-tray.exe" # Not elevated
Write-Host "Hadal is installed. Right-click the helmet in the tray and choose Show pairing QR."
