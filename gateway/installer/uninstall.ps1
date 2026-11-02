# 卸载 mcwave USB 网关：停止程序、删除开机自启和安装文件夹。
param(
  [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'McwaveGateway'),
  [string]$StartupDir = [Environment]::GetFolderPath('Startup')
)
$ErrorActionPreference = 'Continue'
Get-Process -Name McwaveGateway -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$link = Join-Path $StartupDir 'McwaveGateway.lnk'
if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
if (Test-Path -LiteralPath $InstallDir) { Remove-Item -LiteralPath $InstallDir -Recurse -Force }
Write-Host '已卸载 mcwave USB 网关。'
