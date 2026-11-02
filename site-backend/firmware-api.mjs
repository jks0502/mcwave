// V6 OTA: release metadata in CONTROL_DB, image bytes in the FIRMWARE KV
// namespace. A device downloads only the image requested for itself, in
// bounded ranges so a broken campus transfer costs one small retry.
const HEX64=/^[a-f0-9]{64}$/,CHUNK_MAX=16384;
const headers={'Cache-Control':'no-store','Content-Type':'application/json; charset=utf-8','X-Content-Type-Options':'nosniff'};
const json=(body,status=200)=>new Response(JSON.stringify(body),{status,headers});
const fail=(error,status=400,code='INVALID_FIRMWARE')=>json({error,code},status);
const OTA_STATES=['idle','downloading','verifying','rebooting','confirming','installed','failed','rejected'];
export const otaSupported=(env,profile)=>!!(env.FIRMWARE&&profile?.ota);

// Device report field {state,progress,sha256,error}; null rejects the report.
export function normalizeOta(o){
  if(o===undefined)return undefined;
  if(!o||typeof o!=='object'||Array.isArray(o)||!Object.keys(o).every(k=>['state','progress','sha256','error'].includes(k)))return null;
  if(!OTA_STATES.includes(o.state)||!Number.isSafeInteger(o.progress)||o.progress<0||o.progress>100)return null;
  if(!(o.sha256===''||HEX64.test(o.sha256||''))||!/^[a-z0-9]{0,16}$/.test(o.error??''))return null;
  return {state:o.state,progress:o.progress,sha256:o.sha256,error:o.error??''};
}

async function target(env,deviceId,board){
  if(!['mcwave','edison'].includes(board))return null;
  return env.CONTROL_DB.prepare(`SELECT o.target_sha,o.attempt,o.requested_at,f.board,f.version,f.size,f.notes FROM md_device_ota o
    JOIN md_firmware f ON f.sha256=o.target_sha AND f.device_id=o.device_id AND f.board=? WHERE o.device_id=? AND o.target_sha<>''`).bind(board||'',deviceId).first();
}
// Sync/session reply field. A device already running the requested version has
// finished; the request is cleared so a later USB downgrade is not undone.
export async function otaReply(env,row,profile){
  if(!otaSupported(env,profile))return {};
  const t=await target(env,row.id,profile.board);if(!t||!profile.board)return {};
  const report=JSON.parse(row.report_json||'{}');
  if(report.firmware===t.version){
    await env.CONTROL_DB.prepare("UPDATE md_device_ota SET target_sha='' WHERE device_id=? AND target_sha=? AND attempt=?").bind(row.id,t.target_sha,t.attempt).run();
    return {};
  }
  return {ota:{sha256:t.target_sha,board:t.board,version:t.version,size:t.size,attempt:t.attempt}};
}

export async function deviceFirmware(request,env,url,row,profile,sha){
  if(!otaSupported(env,profile))return fail('此设备不支持固件更新',400,'UNSUPPORTED');
  const t=await target(env,row.id,profile.board);
  if(!t||t.target_sha!==sha)return fail('没有要求下载此固件',410,'FIRMWARE_NOT_REQUESTED');
  const offset=Number(url.searchParams.get('offset')),length=Number(url.searchParams.get('length'));
  if(!Number.isSafeInteger(offset)||!Number.isSafeInteger(length)||offset<0||length<1||length>CHUNK_MAX||offset+length>t.size)return fail('下载范围无效',416,'RANGE');
  const image=await env.FIRMWARE.get('fw:'+sha,{type:'arrayBuffer',cacheTtl:3600});
  if(!image||image.byteLength!==t.size)return fail('固件文件不可用',503,'FIRMWARE_MISSING');
  return new Response(request.method==='HEAD'?null:image.slice(offset,offset+length),{headers:{'Cache-Control':'no-store','Content-Type':'application/octet-stream',
    'Content-Length':String(length),'X-Firmware-Size':String(t.size),'X-Firmware-Sha256':sha}});
}

async function snapshot(env,row,profile){
  const report=JSON.parse(row.report_json||'{}'),t=await target(env,row.id,profile.board);
  const releases=(await env.CONTROL_DB.prepare('SELECT sha256,board,version,size,notes,created_at FROM md_firmware WHERE device_id=? AND board=? ORDER BY created_at DESC LIMIT 10').bind(row.id,profile.board||'').all()).results;
  const s=await env.CONTROL_DB.prepare('SELECT attempt FROM md_device_ota WHERE device_id=?').bind(row.id).first();
  return {supported:otaSupported(env,profile),board:profile.board||'',current:report.firmware||'',ota:report.ota||null,attempt:s?.attempt||0,
    online:!!row.enabled&&!!row.session_id&&row.last_seen>Date.now()-90000,
    target:t?{sha256:t.target_sha,board:t.board,version:t.version,size:t.size,notes:t.notes,attempt:t.attempt,requestedAt:t.requested_at}:null,
    releases:releases.map(r=>({sha256:r.sha256,board:r.board,version:r.version,size:r.size,notes:r.notes,createdAt:r.created_at}))};
}
// Web: GET status; POST {action:'install',sha256,baseAttempt} or {action:'cancel',baseAttempt}.
export async function manageFirmware(request,env,row,profile,b){
  if(!otaSupported(env,profile))return request.method==='GET'?json({supported:false}):fail('此设备不支持固件更新',400,'UNSUPPORTED');
  if(request.method==='GET')return json(await snapshot(env,row,profile));
  if(!row.enabled)return fail('设备已停用',409,'DEVICE_DISABLED');
  if(!b||!['install','cancel'].includes(b.action)||!Number.isSafeInteger(b.baseAttempt)||b.baseAttempt<0)return fail('固件操作无效');
  const db=env.CONTROL_DB;let changed;
  if(b.action==='install'){
    if(!Object.keys(b).every(k=>['action','sha256','baseAttempt'].includes(k))||!HEX64.test(b.sha256||''))return fail('固件操作无效');
    const release=await db.prepare('SELECT version,board FROM md_firmware WHERE sha256=? AND device_id=?').bind(b.sha256,row.id).first();
    if(!release)return fail('这台设备没有这个固件',404,'FIRMWARE_UNKNOWN');
    if(!profile.board||release.board!==profile.board)return fail('固件板型与设备不匹配',409,'FIRMWARE_BOARD');
    if(JSON.parse(row.report_json||'{}').firmware===release.version)return fail('设备已经是这个版本',409,'ALREADY_INSTALLED');
    await db.prepare('INSERT OR IGNORE INTO md_device_ota(device_id) VALUES(?)').bind(row.id).run();
    changed=await db.prepare('UPDATE md_device_ota SET target_sha=?,attempt=attempt+1,requested_at=? WHERE device_id=? AND attempt=? RETURNING attempt')
      .bind(b.sha256,Date.now(),row.id,b.baseAttempt).first();
  }else{
    if(!Object.keys(b).every(k=>['action','baseAttempt'].includes(k)))return fail('固件操作无效');
    changed=await db.prepare("UPDATE md_device_ota SET target_sha='' WHERE device_id=? AND attempt=? AND target_sha<>'' RETURNING attempt").bind(row.id,b.baseAttempt).first();
  }
  return changed?json(await snapshot(env,row,profile)):json({error:'更新状态已变化，请重新查看',code:'OTA_CONFLICT',...await snapshot(env,row,profile)},409);
}
