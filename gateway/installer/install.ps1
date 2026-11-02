# mcwave USB 网关安装：当前用户安装，不需要管理员权限。
# 复制 McwaveGateway.exe 到 %LOCALAPPDATA%\McwaveGateway，加入开机自启，并立即启动。
param(
  [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'McwaveGateway'),
  [string]$StartupDir = [Environment]::GetFolderPath('Startup'),
  [switch]$NoStart
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'McwaveGateway.exe'
if (-not (Test-Path -LiteralPath $source)) { throw '找不到 McwaveGateway.exe（请把它和本脚本放在同一个文件夹）' }
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

# 如果旧版正在运行，先停掉再覆盖。
Get-Process -Name McwaveGateway -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$exe = Join-Path $InstallDir 'McwaveGateway.exe'
Copy-Item -LiteralPath $source -Destination $exe -Force

New-Item -ItemType Directory -Force -Path $StartupDir | Out-Null
$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut((Join-Path $StartupDir 'McwaveGateway.lnk'))
$link.TargetPath = $exe
$link.WorkingDirectory = $InstallDir
$link.WindowStyle = 7
$link.Description = 'mcwave USB 网关'
$link.Save()

if (-not $NoStart) { Start-Process -FilePath $exe -WorkingDirectory $InstallDir -WindowStyle Hidden }

$present = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_1A86&PID_55D3*' }
Write-Host ''
Write-Host '安装完成：网关已启动，之后每次开机会自动运行。'
if ($present) { Write-Host '已检测到 mcwave 的 USB 串口，屏幕几秒后会开始更新。' }
else { Write-Host '还没检测到 mcwave：请用能传数据的 USB 线把它插到这台电脑。' }
Write-Host ('运行记录：' + (Join-Path $InstallDir 'gateway.log'))
