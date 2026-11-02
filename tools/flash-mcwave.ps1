param(
  [string]$Port='COM3',
  [string]$BuildDir='',
  [switch]$Layout,
  [string]$Log=''
)
# USB flash for mcwave (ESP32, 4MB). Default: otadata reset (boot_app0) + app at
# 0x10000, like edison. -Layout (one-time V5.4 -> V5.5 migration): also writes
# the bootloader and the OTA partition table, and erases the relocated SPIFFS
# photo cache so it formats cleanly. NVS (settings) is never touched.
$ErrorActionPreference='Stop'
# esptool writes progress to stderr; success is judged from its logs instead.
function Invoke-Esptool([string]$OutFile,[string[]]$Arguments){$ErrorActionPreference='Continue';& $esptool @Arguments *> $OutFile}
$projectDir=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$esptool="$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\5.3.0-cn\esptool.exe"
if(-not $BuildDir){$BuildDir=Join-Path $projectDir 'build\mcwave'}
$app=Join-Path $BuildDir 'WeatherClock.ino.bin';$boot=Join-Path $BuildDir 'WeatherClock.ino.bootloader.bin'
$parts=Join-Path $BuildDir 'WeatherClock.ino.partitions.bin';$bootApp0=Join-Path $BuildDir 'boot_app0.bin'
foreach($f in $app,$bootApp0){if(-not (Test-Path $f)){throw "missing $f"}}
if((Get-Item $app).Length -gt 1835008){throw 'image does not fit an OTA slot'}
if(-not $Log){$Log=Join-Path $projectDir 'logs\mcwave-flash-last.txt'}
Invoke-Esptool "$Log.identity.txt" @('--port',$Port,'--baud','115200','--after','no-reset','read-mac')
$identity=Get-Content -Raw "$Log.identity.txt"
if($identity -notmatch [regex]::Escape('AA:BB:CC:DD:EE:FF')){throw "Board on $Port is not mcwave (MAC AA:BB:CC:DD:EE:FF)"}
$writes=@('0xe000',$bootApp0,'0x10000',$app);$expect=2
if($Layout){
  foreach($f in $boot,$parts){if(-not (Test-Path $f)){throw "missing $f"}}
  $writes=@('0x1000',$boot,'0x8000',$parts)+$writes;$expect=4
  Invoke-Esptool "$Log.erase.txt" @('--chip','esp32','--port',$Port,'--baud','460800','--before','no-reset','--after','no-reset','erase-region','0x390000','0x70000')
  if(-not (Select-String -Path "$Log.erase.txt" -Pattern 'erased successfully|Erase completed|Flash memory erased' -Quiet)){throw "SPIFFS erase failed; see $Log.erase.txt"}
}
# A fresh reset into the ROM bootloader: an earlier step may leave the stub at another baud rate.
Invoke-Esptool $Log (@('--chip','esp32','--port',$Port,'--baud','460800','--before','default-reset','--after','hard-reset','write-flash')+$writes)
$out=Get-Content -Raw $Log
if(([regex]::Matches($out,'Hash of data verified')).Count -lt $expect){throw "Write was not fully verified; see $Log"}
[pscustomobject]@{port=$Port;layout=[bool]$Layout;image=$app;sha256=(Get-FileHash $app).Hash.ToLower();verified=$true;log=$Log}
