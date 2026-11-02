// Transitional transport: only the explicitly bound mcwave uses the legacy queue.
const enabled=env=>env.MULTI_DEVICE_ENABLED==='true';
export const albumCapableFirmware=v=>/^V(?:[5-9]|[1-9][0-9]+)\./.test(v||'');
export async function legacyBinding(env,id){
  if(!enabled(env))return null;
  return env.CONTROL_DB.prepare(`SELECT d.* FROM md_legacy_bridge b JOIN md_devices d ON d.id=b.device_id
    WHERE b.id=1 AND d.space_id='personal' AND d.profile_id='mcwave-hink-v1'${id?' AND d.id=?':''}`).bind(...(id?[id]:[])).first();
}
export async function legacyState(env,row,bound){
  const binding=bound===undefined?await legacyBinding(env,row.id):bound;
  if(!binding||binding.id!==row.id)return null;
  const old=await env.CONTROL_DB.prepare('SELECT * FROM epaper_control WHERE id=1').first();
  if(!old)throw Error('legacy state missing');
  const {ota,supportedModes}=JSON.parse(row.report_json||'{}');
  const report={mode:old.reported_mode,page:old.reported_page,pageCount:old.page_count,firmware:old.firmware,error:'',...(ota?{ota}:{}),...(supportedModes?{supportedModes}:{})};
  return {...row,revision:old.revision,applied_revision:old.applied_revision,
    requested_at:old.requested_at,desired_json:JSON.stringify({mode:old.mode,page:old.page}),report_json:JSON.stringify(report),last_seen:old.last_seen,
    legacy:true,session_id:old.last_seen?'legacy':'',confirmed_session:old.last_seen?'legacy':'',next_wake_at:0};
}
export async function authorizeLegacyDevice(request,env){
  const token=/^Bearer ([a-f0-9]{64})$/.exec(request.headers.get('Authorization')||'')?.[1];
  if(!token)return false;
  const bridge=await legacyBinding(env);
  if(bridge&&!bridge.enabled)return false;
  if(bridge){
    const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(token))),b=>b.toString(16).padStart(2,'0')).join('');
    // An indexed digest lookup binds the credential to the one legacy device.
    const own=await env.CONTROL_DB.prepare('SELECT id FROM md_devices WHERE id=? AND token_hash=? AND enabled=1').bind(bridge.id,digest).first();
    if(own)return true;
  }
  if(env.LEGACY_DEVICE_TOKEN_ENABLED==='false'||!env.DEVICE_TOKEN||env.DEVICE_TOKEN.length<32)return false;
  const encoder=new TextEncoder(),key=await crypto.subtle.importKey('raw',encoder.encode(env.DEVICE_TOKEN),{name:'HMAC',hash:'SHA-256'},false,['sign','verify']);
  const mac=await crypto.subtle.sign('HMAC',key,encoder.encode(env.DEVICE_TOKEN));
  return crypto.subtle.verify('HMAC',key,mac,encoder.encode(token));
}
