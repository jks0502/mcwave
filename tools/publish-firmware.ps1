param(
  [Parameter(Mandatory)][ValidateSet('mcwave','edison')][string]$Board,
  [Parameter(Mandatory)][string]$Bin,
  [Parameter(Mandatory)][string]$Version,
  [string]$Notes='',
  [switch]$DryRun
)
$ErrorActionPreference='Stop'
$Board=$Board.ToLowerInvariant()
$map=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'firmware-boards.json') | ConvertFrom-Json
$target=$map.$Board
$deviceId=$target.deviceId
$bin=(Resolve-Path -LiteralPath $Bin).Path
if($Version -notmatch '^V[0-9A-Za-z.-]{1,31}$'){throw 'Version must be an exact V-prefixed firmware version'}
if($Notes.Length -gt 200 -or $Notes -match '[\x00-\x1f]'){throw 'Notes must be one line of at most 200 characters'}
$bytes=[IO.File]::ReadAllBytes($bin)
if($bytes.Length -lt 1024 -or $bytes.Length -gt $target.maximumSize){throw 'Image does not fit this board application slot'}
if($bytes[0] -ne 0xe9){throw 'Invalid ESP application magic'}
$chip=[int]$bytes[12]+256*[int]$bytes[13]
if($chip -ne $target.chipId){throw "Image chip id $chip does not match $Board ($($target.chipId))"}
$text=[Text.Encoding]::GetEncoding(28591).GetString($bytes)
$marker="EPAPER-BOARD:$Board"+[char]0
if($text.IndexOf($marker,[StringComparison]::Ordinal) -lt 0){throw "Missing $Board firmware marker"}
foreach($other in 'mcwave','edison'){
  if($other -ne $Board -and $text.IndexOf("EPAPER-BOARD:$other"+[char]0,[StringComparison]::Ordinal) -ge 0){throw 'Image also contains another board marker'}
}
if($text.IndexOf($Version+[char]0,[StringComparison]::Ordinal) -lt 0){throw 'Image does not contain the exact declared version'}
$sha=(Get-FileHash -LiteralPath $bin -Algorithm SHA256).Hash.ToLower()
$result=[pscustomobject]@{board=$Board;device=$deviceId;chipId=$chip;version=$Version;label="$Board $Version";size=$bytes.Length;sha256=$sha;dryRun=[bool]$DryRun}
# All image validation is offline and precedes even a database query or KV upload.
if($DryRun){$result;return}
$site=(Resolve-Path (Join-Path $PSScriptRoot '..\personal-site')).Path
$config=Join-Path $site 'wrangler.v6.production.jsonc'
$wrangler=Join-Path $site 'node_modules\wrangler\bin\wrangler.js'
function Query([string]$Sql){
  $out=& node $wrangler d1 execute epaper-control --remote --config $config --json --command $Sql
  if($LASTEXITCODE){throw 'D1 query failed (board migration must precede publishing)'}
  $parsed=($out -join "`n") | ConvertFrom-Json
  return $parsed[0].results
}
Push-Location $site
try{
  $device=@(Query "SELECT id,profile_id FROM md_devices WHERE id='$deviceId'")
  if($device.Count -ne 1 -or $device[0].profile_id -notin $target.profiles){throw 'Device mapping/profile does not match the selected board'}
  $existing=@(Query "SELECT device_id,board,version,size FROM md_firmware WHERE sha256='$sha'")
  if($existing.Count -and ($existing[0].device_id -ne $deviceId -or $existing[0].board -ne $Board -or $existing[0].version -ne $Version -or $existing[0].size -ne $bytes.Length)){throw 'SHA already registered with different device/board/version metadata'}
  $ns=[regex]::Match((Get-Content -Raw $config),'"binding"\s*:\s*"FIRMWARE"\s*,\s*"id"\s*:\s*"([0-9a-f]{32})"').Groups[1].Value
  if(-not $ns){throw 'FIRMWARE KV namespace is not configured'}
  & node $wrangler kv key put "fw:$sha" --path $bin --namespace-id $ns --remote | Out-Null
  if($LASTEXITCODE){throw 'KV upload failed'}
  $note=$Notes.Replace("'","''")
  Query "INSERT INTO md_firmware(sha256,device_id,board,version,size,notes,created_at) VALUES('$sha','$deviceId','$Board','$Version',$($bytes.Length),'$note',$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())) ON CONFLICT(sha256) DO NOTHING" | Out-Null
  $registered=@(Query "SELECT device_id,board,version,size FROM md_firmware WHERE sha256='$sha'")
  if($registered.Count -ne 1 -or $registered[0].device_id -ne $deviceId -or $registered[0].board -ne $Board -or $registered[0].version -ne $Version -or $registered[0].size -ne $bytes.Length){throw 'Release registration verification failed'}
}finally{Pop-Location}
$result
