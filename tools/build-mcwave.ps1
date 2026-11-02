param(
  [string]$Cli='D:\arduino\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe',
  [string]$Name='mcwave',
  [string[]]$Defines=@('MCWAVE_MULTI_NETWORK')
)
# mcwave V5.5+: WeatherClock/partitions.csv (two 1.75MB OTA slots) replaces
# huge_app. The size limit is enforced at link time so an image that would not
# fit an OTA slot never builds. Output: build\<Name>, firmware\<Name>.
$ErrorActionPreference='Stop'
$projectDir=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
foreach($f in 'Secrets.h','RemoteSecrets.h','partitions.csv'){if(-not (Test-Path -LiteralPath (Join-Path $projectDir "WeatherClock\$f"))){throw "WeatherClock\$f is required"}}
if($Defines -contains 'MCWAVE_MULTI_NETWORK'){
  foreach($f in 'CampusSecrets.h','NjitSecrets.h','MultiNetworkSecrets.h'){
    if(-not (Test-Path -LiteralPath (Join-Path $projectDir "WeatherClock\$f"))){throw "WeatherClock\$f is required for multi-network mcwave"}
  }
}
if(($Defines | Where-Object {$_ -ne 'MCWAVE_MULTI_NETWORK'}) -and $Name -eq 'mcwave'){throw 'Diagnostic definitions require a separate -Name'}
$extra=@()
if($Defines.Count){$extra=@('--build-property',('compiler.cpp.extra_flags='+(($Defines|ForEach-Object{"-D$_"}) -join ' ')))}
& $Cli compile --jobs 4 --fqbn 'esp32:esp32:esp32:PartitionScheme=huge_app' `
  --config-file (Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml') `
  --build-path (Join-Path $projectDir "build\$Name") --output-dir (Join-Path $projectDir "firmware\$Name") `
  --build-property 'upload.maximum_size=1835008' `
  --build-property 'compiler.c.elf.extra_flags=-Wl,--wrap=vApplicationGetTimerTaskMemory,--wrap=dns_clear_cache,--wrap=esp_mbedtls_mem_calloc,--wrap=esp_mbedtls_mem_free' `
  @extra (Join-Path $projectDir 'WeatherClock') 2>&1 | Tee-Object -FilePath (Join-Path $projectDir "logs\$Name-build.log")
if ($LASTEXITCODE -ne 0) { throw "mcwave compile failed; see logs\$Name-build.log" }
$parts=Join-Path $projectDir "build\$Name\WeatherClock.ino.partitions.bin"
$csv=(Get-Content -Raw (Join-Path $projectDir 'WeatherClock\partitions.csv'))
if(-not (Test-Path $parts)){throw 'partition table was not generated'}
if($csv -notmatch 'ota_1'){throw 'partitions.csv is not the OTA layout'}
