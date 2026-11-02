import {commitTarget} from './device-target.mjs';
import {multiAlbumEnabled,manageDeviceAlbum,activateDeviceAlbum,reconcileDeviceAlbum,deviceAlbumResource,photoUsage} from './multi-album-api.mjs';
import {legacyBinding,legacyState,authorizeLegacyDevice} from './legacy-device-bridge.mjs';
import {putScreen,getScreen} from './screen-mirror-api.mjs';
import {otaReply,deviceFirmware,manageFirmware,normalizeOta} from './firmware-api.mjs';
import {moodVersions,moodDevice} from './mood-api.mjs';
// V6.0 protocol 2. Persistent state lives in D1; no request state is cached here.
const SPACE='personal',UINT32=4294967295,HEX32=/^[a-f0-9]{32}$/;
const PROFILES=Object.freeze({
  // mcwave 6.0.0 replaces the sports page with the two-person mood page.
  'mcwave-hink-v1':Object.freeze({board:'mcwave',hardware:'ESP32 / Rev3 / HINK 400x300',modes:['weather','mood','album'],commandModes:['weather','mood'],firstMode:'weather',minimumRefreshSeconds:60,cacheLimit:20,ota:true}),
  'edison-hink-s3-v1':Object.freeze({board:'edison',hardware:'ESP32-S3 N16R8 / V1.5 / HINK 400x300',modes:['diagnostic'],commandModes:['diagnostic'],firstMode:'diagnostic',minimumRefreshSeconds:60,cacheLimit:0}),
  // V6.6 adds mcwave's weather/sports pages; the device's supportedModes report narrows what the page offers.
  'edison-hink-s3-album-v1':Object.freeze({board:'edison',hardware:'ESP32-S3 N16R8 / V1.5 / HINK 400x300',modes:['diagnostic','weather','sports','album','mood'],commandModes:['diagnostic','weather','sports','mood'],firstMode:'diagnostic',minimumRefreshSeconds:60,cacheLimit:20,ota:true})
});
const headers={'Cache-Control':'no-store','Content-Type':'application/json; charset=utf-8','X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer'};
const json=(body,status=200)=>new Response(JSON.stringify(body),{status,headers});
const fail=(error,status=400,code='INVALID_REQUEST')=>json({error,code},status);
export const multiDeviceEnabled=env=>env.MULTI_DEVICE_ENABLED==='true';
export const multiDeviceWebPath=path=>path==='/api/epaper/devices'||path.startsWith('/api/epaper/devices/');
const integer=(n,max=UINT32)=>Number.isSafeInteger(n)&&n>=0&&n<=max;
const keys=(o,allowed)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).every(k=>allowed.includes(k));
const cleanName=s=>typeof s==='string'&&Array.from(s.trim()).length>=1&&Array.from(s.trim()).length<=24&&!/[\u0000-\u001f\u007f]/.test(s);
const randomHex=n=>Array.from(crypto.getRandomValues(new Uint8Array(n)),b=>b.toString(16).padStart(2,'0')).join('');
async function hash(s){return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(s))),b=>b.toString(16).padStart(2,'0')).join('');}
async function body(request){
  if(!request.headers.get('Content-Type')?.startsWith('application/json')||Number(request.headers.get('Content-Length')||0)>2048)return null;
  const reader=request.body?.getReader();if(!reader)return null;const buffer=new Uint8Array(2048);let size=0;
  try{for(;;){const r=await reader.read();if(r.done)break;if(size+r.value.length>2048){await reader.cancel();return null;}buffer.set(r.value,size);size+=r.value.length;}const o=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(buffer.subarray(0,size)));return keys(o,Object.keys(o||{}))?o:null;}catch{return null;}finally{reader.releaseLock();}
}
const statement=(env,sql,...args)=>env.CONTROL_DB.prepare(sql).bind(...args);
const get=(env,id)=>statement(env,'SELECT * FROM md_devices WHERE id=? AND space_id=?',id,SPACE).first();
function publicDevice(row,env){
  const profile=PROFILES[row.profile_id],report=JSON.parse(row.report_json),online=!!row.enabled&&!!row.session_id&&row.last_seen>Date.now()-90000;
  return {id:row.id,name:row.name,profileId:row.profile_id,enabled:!!row.enabled,metadataRevision:row.metadata_revision,
    revision:row.revision,appliedRevision:row.applied_revision,desired:JSON.parse(row.desired_json),
    reported:report.mode?{mode:report.mode,page:report.page,pageCount:report.pageCount,firmware:report.firmware,error:report.error,...(report.photoId?{photoId:report.photoId}:{})}:null,
    lastSeen:row.last_seen,nextWakeAt:row.next_wake_at,online,
    status:!row.enabled?'disabled':row.next_wake_at>Date.now()?'sleeping':online?'online':row.last_seen?'offline':'awaiting',
    pending:row.applied_revision<row.revision||!row.session_id||row.confirmed_session!==row.session_id,
    transport:row.legacy?'legacy':'v2',capabilities:{...profile,modes:report.supportedModes||profile.modes,commandModes:(row.legacy||multiAlbumEnabled(env)?profile.modes:profile.commandModes).filter(m=>(report.supportedModes||profile.modes).includes(m))},sessionGeneration:row.session_generation};
}
const deviceReply=row=>({protocol:2,deviceId:row.id,revision:row.revision,appliedRevision:row.applied_revision,
  desired:JSON.parse(row.desired_json),needsApply:row.applied_revision<row.revision||row.confirmed_session!==row.session_id,syncAfterSeconds:15});
async function limited(binding,key){if(!binding)return false;return (await binding.limit({key})).success;}
// One target-validation path for browser sessions and scoped controllers.
async function setDesired(request,env,url,row,b,handlers={}){
  if(await legacyBinding(env,row.id)){
    if(!row.enabled)return fail('设备已停用',409,'DEVICE_DISABLED');
    if(!keys(b,['baseRevision','desired'])||!integer(b.baseRevision)||!keys(b.desired,['mode','page'])||!PROFILES[row.profile_id].modes.includes(b.desired.mode)||!(JSON.parse(row.report_json).supportedModes||PROFILES[row.profile_id].modes).includes(b.desired.mode))return fail('设备不支持此设置');
    const response=await handlers.control(new Request(request.url,{method:'POST',headers:request.headers,body:JSON.stringify({...b.desired,baseRevision:b.baseRevision})}),env,new URL('/api/epaper/command',url),{cooldownMs:handlers.controllerCooldownMs||0});
    const result=await response.json();
    return json({...(result.error?{error:result.error,code:response.status===409?'REVISION_CONFLICT':'INVALID_REQUEST'}:{}),device:publicDevice(await legacyState(env,await get(env,row.id)),env)},response.status);
  }
  {
    if(!keys(b,['baseRevision','desired'])||!integer(b.baseRevision))return fail('目标版本无效');
    if(!row.enabled)return fail('设备已停用',409,'DEVICE_DISABLED');
    const desired=b.desired,p=PROFILES[row.profile_id],declared=JSON.parse(row.report_json).supportedModes||p.modes;
    if(!keys(desired,['mode','page'])||!p.modes.includes(desired.mode)||!declared.includes(desired.mode)||!integer(desired.page,63)||desired.mode!=='sports'&&desired.page!==0)return fail('设备不支持此设置');
    if(desired.mode==='album'&&multiAlbumEnabled(env)){
      const result=await activateDeviceAlbum(env,row,b.baseRevision,{cooldownMs:handlers.controllerCooldownMs||0});if(result.status!==200)return result;
      return json({device:publicDevice(await get(env,row.id),env)});
    }
    if(!p.commandModes.includes(desired.mode))return fail('新版独立相册接口将在相册阶段接入，原相册不受影响',409,'ALBUM_V2_PENDING');
    if(b.baseRevision===UINT32)return fail('指令序号已达上限',409,'REVISION_LIMIT');
    const updated=await commitTarget(env,row,b.baseRevision,{mode:desired.mode,page:desired.page},{cooldownMs:handlers.controllerCooldownMs||0});
    return updated?json({device:publicDevice(updated,env)}):json({error:'设备设置已更新，请重新选择',code:'REVISION_CONFLICT',device:publicDevice(await get(env,row.id),env)},409);
  }
}
async function web(request,env,url,handlers){
  const session=/epaper_session=[vm]1\.\d+\.([a-f0-9-]{36})\./.exec(request.headers.get('Cookie')||'')?.[1];
  if(!await limited(env.CONTROL_LIMITER,'md-web:'+SPACE+':'+(session||'unknown')))return fail('操作较频繁，请稍后重试',429,'RATE_LIMIT');
  if(url.pathname==='/api/epaper/devices'){
    if(request.method==='GET'){
      const rows=await statement(env,'SELECT * FROM md_devices WHERE space_id=? ORDER BY created_at,id LIMIT 101',SPACE).all();
      const bridge=await legacyBinding(env);
      return json({version:'V6.0',devices:await Promise.all(rows.results.slice(0,100).map(async row=>publicDevice(await legacyState(env,row,bridge)||row,env))),hasMore:rows.results.length>100});
    }
    if(request.method!=='POST')return fail('不支持的操作',405);
    const b=await body(request);if(!keys(b,['name','profileId'])||!cleanName(b.name)||!Object.hasOwn(PROFILES,b.profileId))return fail('请指定有效名称和已支持的硬件配置');
    const id=crypto.randomUUID(),token=randomHex(32),profile=PROFILES[b.profileId];
    const row=await statement(env,`INSERT INTO md_devices(id,space_id,name,profile_id,token_hash,desired_json,created_at)
      SELECT ?,?,?,?,?,?,? WHERE (SELECT count(*) FROM md_devices WHERE space_id=?)<100 RETURNING *`,
      id,SPACE,b.name.trim(),b.profileId,await hash(token),JSON.stringify({mode:profile.firstMode,page:0}),Date.now(),SPACE).first();
    return row?json({device:publicDevice(row,env),token,tokenShownOnce:true},201):fail('当前设备数量已达上限',409,'DEVICE_LIMIT');
  }
  const usage=/^\/api\/epaper\/devices\/photo-usage\/([a-f0-9]{64})$/.exec(url.pathname);
  if(usage)return request.method==='GET'?photoUsage(env,usage[1]):fail('不支持的操作',405);
  const screenPath=/^\/api\/epaper\/devices\/([a-f0-9-]{36})\/screen$/.exec(url.pathname);
  if(screenPath){const row=await get(env,screenPath[1]);return row?getScreen(request,env,row):fail('未找到',404);}
  const firmwarePath=/^\/api\/epaper\/devices\/([a-f0-9-]{36})\/firmware$/.exec(url.pathname);
  if(firmwarePath){
    const row=await get(env,firmwarePath[1]);if(!row)return fail('未找到',404);
    if(!['GET','POST'].includes(request.method))return fail('不支持的操作',405);
    return manageFirmware(request,env,await legacyState(env,row)||row,PROFILES[row.profile_id],request.method==='POST'?await body(request):null);
  }
  const albumPath=/^\/api\/epaper\/devices\/([a-f0-9-]{36})\/album$/.exec(url.pathname);
  if(albumPath){const row=await get(env,albumPath[1]);return row?manageDeviceAlbum(request,env,url,row):fail('not found',404);}
  const match=/^\/api\/epaper\/devices\/([a-f0-9-]{36})\/(state|desired|name|credential|enabled)$/.exec(url.pathname);
  if(!match)return fail('未找到',404);
  const row=await get(env,match[1]);if(!row)return fail('未找到',404);
  if(match[2]==='state'&&request.method==='GET')return json({device:publicDevice(await legacyState(env,row)||row,env)});
  if(request.method!=='POST'||match[2]==='state')return fail('不支持的操作',405);
  const b=await body(request);if(!b)return fail('请求格式或长度不正确');
  if(match[2]==='desired')return setDesired(request,env,url,row,b,handlers);
  if(!integer(b.baseMetadataRevision)||b.baseMetadataRevision!==row.metadata_revision)return fail('设备资料已更新，请重新读取',409,'METADATA_CONFLICT');
  let updated,token;
  if(match[2]==='name'){
    if(!keys(b,['baseMetadataRevision','name'])||!cleanName(b.name))return fail('名称应为1至24个字符');
    updated=await statement(env,'UPDATE md_devices SET name=?,metadata_revision=metadata_revision+1 WHERE id=? AND space_id=? AND metadata_revision=? RETURNING *',b.name.trim(),row.id,SPACE,b.baseMetadataRevision).first();
  }else if(match[2]==='enabled'){
    if(!keys(b,['baseMetadataRevision','enabled'])||typeof b.enabled!=='boolean')return fail('启用状态无效');
    updated=await statement(env,'UPDATE md_devices SET enabled=?,metadata_revision=metadata_revision+1 WHERE id=? AND space_id=? AND metadata_revision=? RETURNING *',Number(b.enabled),row.id,SPACE,b.baseMetadataRevision).first();
  }else{
    if(!keys(b,['baseMetadataRevision']))return fail('密钥请求无效');token=randomHex(32);
    updated=await statement(env,`UPDATE md_devices SET token_hash=?,metadata_revision=metadata_revision+1,session_id='',confirmed_session='',boot_id='',last_sequence=0
      WHERE id=? AND space_id=? AND metadata_revision=? RETURNING *`,await hash(token),row.id,SPACE,b.baseMetadataRevision).first();
  }
  return updated?json({device:publicDevice(await legacyState(env,updated)||updated,env),...(token?{token,tokenShownOnce:true}:{})}):fail('设备资料已更新，请重新读取',409,'METADATA_CONFLICT');
}
function normalizeReport(b,profile){
  if(!keys(b,['protocol','sessionId','sequence','appliedRevision','mode','page','pageCount','firmware','error','nextWakeAt','supportedModes','photoId','ota'])||b.protocol!==2||!HEX32.test(b.sessionId||'')||!integer(b.sequence)||b.sequence<1||!integer(b.appliedRevision)||!profile.modes.includes(b.mode)||!integer(b.page,63)||!integer(b.pageCount,64)||b.pageCount<1||b.page>=b.pageCount||b.mode!=='sports'&&b.page!==0||!/^V[0-9A-Za-z.-]{1,31}$/.test(b.firmware||''))return null;
  if(b.mode==='album'?!/^[a-f0-9]{64}$/.test(b.photoId||''):b.photoId!==undefined&&b.photoId!=='')return null;
  const error=b.error??'',wake=b.nextWakeAt??0;
  if(!['','network','render','asset','unsupported','mood-partial'].includes(error)||error==='mood-partial'&&b.mode!=='mood'||!integer(wake,Number.MAX_SAFE_INTEGER)||wake!==0&&(wake<Date.now()||wake>Date.now()+86400000))return null;
  if(b.supportedModes!==undefined&&(!Array.isArray(b.supportedModes)||b.supportedModes.length<1||b.supportedModes.length>profile.modes.length||new Set(b.supportedModes).size!==b.supportedModes.length||b.supportedModes.some(m=>!profile.modes.includes(m))||!b.supportedModes.includes(b.mode)))return null;
  const ota=normalizeOta(b.ota);if(ota===null)return null;
  return {protocol:2,sessionId:b.sessionId,sequence:b.sequence,appliedRevision:b.appliedRevision,mode:b.mode,page:b.page,pageCount:b.pageCount,firmware:b.firmware,error,nextWakeAt:wake,...(b.mode==='album'?{photoId:b.photoId}:{}),...(b.supportedModes?{supportedModes:[...b.supportedModes].sort()}:{}),...(ota?{ota}:{})};
}
async function device(request,env,url){
  if(url.protocol!=='https:'&&!['localhost','127.0.0.1','[::1]'].includes(url.hostname))return fail('必须使用HTTPS',400);
  const resource=url.pathname.startsWith('/api/v2/device/album/'),firmware=/^\/api\/v2\/device\/firmware\/([a-f0-9]{64})$/.exec(url.pathname);
  const mood=/^\/api\/v2\/device\/mood\/([01])$/.exec(url.pathname);
  const screen=url.pathname==='/api/v2/device/screen';
  if(resource&&!multiAlbumEnabled(env))return fail('未找到',404);
  if(request.method!=='POST'&&!((resource||firmware)&&['GET','HEAD'].includes(request.method))&&!(mood&&request.method==='GET')&&!(screen&&request.method==='PUT'))return fail('不支持的操作',405);
  const token=/^Bearer ([a-f0-9]{64})$/.exec(request.headers.get('Authorization')||'')?.[1];
  if(!await limited(env.CONTROL_LIMITER,'md-auth:'+(request.headers.get('CF-Connecting-IP')||'local')))return fail('请求较频繁',429,'RATE_LIMIT');
  if(!token)return fail('设备凭证无效',401,'UNAUTHORIZED');
  const tokenHash=await hash(token);let row=await statement(env,'SELECT * FROM md_devices WHERE token_hash=? AND enabled=1 AND space_id=?',tokenHash,SPACE).first();
  // mcwave still uses its provisioned legacy credential. The explicit bridge,
  // never a caller-selected id, scopes this one new resource to that device.
  if(!row&&screen&&await authorizeLegacyDevice(request,env))row=await legacyBinding(env);
  if(!row)return fail('设备凭证无效',401,'UNAUTHORIZED');
  if(screen){
    if(request.method!=='PUT')return fail('不支持的操作',405);
    if(!await limited(env.DEVICE_LIMITER,'md-screen:'+row.id))return fail('设备请求较频繁',429,'RATE_LIMIT');
    return putScreen(request,env,row,PROFILES[row.profile_id].modes);
  }
  if(await legacyBinding(env,row.id))return fail('此设备使用兼容传输，请使用 /api/device/sync',409,'LEGACY_TRANSPORT');
  if(mood){
    if(request.method!=='GET')return fail('不支持的操作',405);
    if(row.profile_id!=='edison-hink-s3-album-v1')return fail('设备不支持此资源',403,'UNSUPPORTED');
    if(!env.ALBUM_LIMITER)return fail('下载限流尚未配置',503);
    if(!await limited(env.ALBUM_LIMITER,'md-mood:'+row.id))return fail('请求较频繁',429,'RATE_LIMIT');
    const response=await moodDevice(env,Number(mood[1]));
    if(response.status===200)response.headers.set('Content-Length','7500');
    return response;
  }
  if(resource){if(!env.ALBUM_LIMITER)return fail('相册限流尚未配置',503);if(!await limited(env.ALBUM_LIMITER,'md-album:'+row.id))return fail('请求较频繁',429);return deviceAlbumResource(request,env,url,row);}
  if(firmware){if(!env.ALBUM_LIMITER)return fail('下载限流尚未配置',503);if(!await limited(env.ALBUM_LIMITER,'md-fw:'+row.id))return fail('请求较频繁',429,'RATE_LIMIT');return deviceFirmware(request,env,url,row,PROFILES[row.profile_id],firmware[1]);}
  // Every successful session/sync reply also carries a pending OTA request.
  const reply=async(r,extra={})=>{
    let mood={};
    try{mood={mood:await moodVersions(env)};}catch{console.error(JSON.stringify({event:'mood_versions_failed'}));}
    return json({...deviceReply(r),...extra,...await otaReply(env,r,PROFILES[r.profile_id]),...mood});
  };
  if(multiAlbumEnabled(env)&&JSON.parse(row.desired_json).mode==='album')row=await reconcileDeviceAlbum(env,row);
  if(!await limited(env.DEVICE_LIMITER,'md-device:'+row.id))return fail('设备请求较频繁',429,'RATE_LIMIT');
  const b=await body(request);if(!b)return fail('请求格式或长度不正确');
  if(url.pathname==='/api/v2/device/session'){
    if(!keys(b,['protocol','bootId'])||b.protocol!==2||!HEX32.test(b.bootId||''))return fail('启动请求无效');
    if(row.boot_id===b.bootId&&row.session_id)return reply(row,{sessionId:row.session_id});
    const sessionId=randomHex(16);
    const updated=await statement(env,`UPDATE md_devices SET boot_id=?,session_id=?,confirmed_session='',last_sequence=0,report_hash='',session_generation=session_generation+1,last_seen=?,next_wake_at=0
      WHERE id=? AND token_hash=? AND enabled=1 AND session_id=? RETURNING *`,b.bootId,sessionId,Date.now(),row.id,tokenHash,row.session_id).first();
    return updated?reply(updated,{sessionId}):fail('启动状态已变化，请重试',409,'SESSION_CONFLICT');
  }
  if(url.pathname!=='/api/v2/device/sync')return fail('未找到',404);
  const report=normalizeReport(b,PROFILES[row.profile_id]);if(!report)return fail('上报字段或设备能力无效');
  const previousModes=JSON.parse(row.report_json).supportedModes;
  if(!report.supportedModes&&previousModes)report.supportedModes=previousModes;
  if(report.supportedModes&&!report.supportedModes.includes(report.mode))return fail('上报模式不在设备声明能力内');
  if(report.sessionId!==row.session_id)return fail('启动会话已变化，请重新握手',409,'STALE_SESSION');
  const text=JSON.stringify(report),reportHash=await hash(text);
  if(report.sequence===row.last_sequence&&reportHash===row.report_hash)return reply(row);
  if(report.sequence<=row.last_sequence)return fail('上报顺序已过期',409,'STALE_SEQUENCE');
  const recovery=(error,code)=>json({error,code,...deviceReply(row)},409);
  if(report.appliedRevision>row.revision)return recovery('回执超过目标版本','FUTURE_ACK');
  if(report.appliedRevision<row.applied_revision)return recovery('回执版本已过期，请应用最新目标','ACK_REGRESSION');
  let confirmed=row.confirmed_session,applied=row.applied_revision;
  if((!report.error||report.error==='mood-partial')&&(report.appliedRevision>row.applied_revision||confirmed!==row.session_id)){
    let target;
    if(report.appliedRevision===row.revision)target=JSON.parse(row.desired_json);
    else if(report.appliedRevision>0){const saved=await statement(env,'SELECT desired_json FROM md_commands WHERE device_id=? AND revision=?',row.id,report.appliedRevision).first();if(saved)target=JSON.parse(saved.desired_json);}
    if(!target||target.mode!==report.mode||target.page!==report.page||target.mode==='album'&&target.photoId!==report.photoId)return recovery('画面与回执不匹配，请读取最新目标','ACK_MISMATCH');
    confirmed=row.session_id;applied=report.appliedRevision;
  }
  const updated=await statement(env,`UPDATE md_devices SET report_json=?,report_hash=?,applied_revision=?,confirmed_session=?,last_sequence=?,last_seen=?,next_wake_at=?
    WHERE id=? AND token_hash=? AND enabled=1 AND session_id=? AND last_sequence=? AND applied_revision<=? AND revision>=? RETURNING *`,
    text,reportHash,applied,confirmed,report.sequence,Date.now(),report.nextWakeAt,row.id,tokenHash,row.session_id,row.last_sequence,applied,report.appliedRevision).first();
  return updated?reply(updated):fail('设备状态已变化，请重新同步',409,'REPORT_CONFLICT');
}
async function guarded(request,env,url,handler){
  if(!multiDeviceEnabled(env))return fail('未找到',404);
  if(!env.CONTROL_DB||!env.CONTROL_LIMITER||!env.DEVICE_LIMITER)return fail('多设备服务尚未配置',503,'NOT_CONFIGURED');
  try{return await handler(request,env,url);}catch{console.error(JSON.stringify({event:'multi_device_service_failed'}));return fail('多设备服务暂时不可用',503,'SERVICE_UNAVAILABLE');}
}
// mcwave (legacy /api/device/*): its OTA report lives on its md_devices row;
// the sync reply carries the same offer as V2. Invalid OTA fields are ignored so
// a bad report never costs the display sync.
export async function legacyOtaReply(env,bridge,firmware,o){
  const ota=normalizeOta(o)||undefined,text=JSON.stringify({firmware,...(ota?{ota}:{})});
  if(bridge.report_json!==text)await statement(env,'UPDATE md_devices SET report_json=? WHERE id=?',text,bridge.id).run();
  return otaReply(env,{id:bridge.id,report_json:text},PROFILES[bridge.profile_id]);
}
export async function legacyFirmware(request,env,url,bridge,sha){
  if(!['GET','HEAD'].includes(request.method))return fail('不支持的操作',405);
  if(!env.ALBUM_LIMITER)return fail('下载限流尚未配置',503);
  if(!await limited(env.ALBUM_LIMITER,'md-fw:'+bridge.id))return fail('请求较频繁',429,'RATE_LIMIT');
  return deviceFirmware(request,env,url,bridge,PROFILES[bridge.profile_id],sha);
}
// The web entry point is invoked only after the existing server-side session/Origin checks.
export const multiDeviceWeb=(request,env,url,handlers={})=>guarded(request,env,url,(r,e,u)=>web(r,e,u,handlers));
export const multiDeviceSync=(request,env,url)=>guarded(request,env,url,device);

// Bearer credentials are SHA-256 digests in D1, never browser sessions or device tokens.
async function controllerDevice(row,env){
  const d=publicDevice(row,env);
  const screen=await statement(env,'SELECT hash,mode,updated_at FROM md_screens WHERE device_id=?',row.id).first();
  return {id:d.id,name:d.name,enabled:d.enabled,revision:d.revision,appliedRevision:d.appliedRevision,desired:d.desired,reported:d.reported,pending:d.pending,lastSeen:d.lastSeen,online:d.online,modes:d.capabilities.commandModes,screen:screen?{hash:screen.hash,mode:screen.mode,updatedAt:screen.updated_at}:null};
}
async function controller(request,env,url,handlers){
  const local=['127.0.0.1','localhost','[::1]'].includes(url.hostname);
  if(url.protocol!=='https:'&&!local)return fail('HTTPS required',400);
  const token=/^Bearer ([a-f0-9]{64})$/.exec(request.headers.get('Authorization')||'')?.[1];
  if(!token)return fail('控制器令牌无效',401,'UNAUTHORIZED');
  const ctl=await statement(env,'SELECT id,allowed_device_ids_json FROM md_controllers WHERE token_hash=? AND enabled=1',await hash(token)).first();
  if(!ctl)return fail('控制器令牌无效',401,'UNAUTHORIZED');
  const allowed=JSON.parse(ctl.allowed_device_ids_json);
  if(!Array.isArray(allowed)||allowed.length>16||allowed.some(id=>typeof id!=='string'||!/^[a-f0-9-]{36}$/.test(id)))throw Error('invalid controller scope');
  const screen=/^\/api\/v2\/controller\/devices\/([a-f0-9-]{36})\/screen$/.exec(url.pathname);
  if(screen){
    if(!allowed.includes(screen[1]))return fail('无权操作此设备',403,'FORBIDDEN');
    if(request.method!=='GET')return fail('不支持的操作',405);
    if(!await limited(env.CONTROLLER_SCREEN_LIMITER,'md-controller-screen:'+ctl.id))return fail('画面请求较频繁',429,'RATE_LIMIT');
    const row=await get(env,screen[1]);return row?getScreen(request,env,row,{allowRanges:true}):fail('未找到',404);
  }
  if(!await limited(env.CONTROL_LIMITER,'md-controller:'+ctl.id))return fail('操作较频繁，请稍后重试',429,'RATE_LIMIT');
  if(url.pathname==='/api/v2/controller/devices'){
    if(request.method!=='GET')return fail('不支持的操作',405);
    const devices=[];
    for(const id of allowed){const row=await get(env,id);if(row)devices.push(await controllerDevice(await legacyState(env,row)||row,env));}
    return json({devices});
  }
  const match=/^\/api\/v2\/controller\/devices\/([a-f0-9-]{36})\/desired$/.exec(url.pathname);
  if(!match)return fail('未找到',404);
  if(!allowed.includes(match[1]))return fail('无权操作此设备',403,'FORBIDDEN');
  if(request.method!=='POST')return fail('不支持的操作',405);
  const row=await get(env,match[1]);if(!row)return fail('未找到',404);
  const b=await body(request);if(!b)return fail('请求格式或长度不正确');
  // Existing requested_at is checked again inside commitTarget's atomic CAS.
  const cooling=current=>Date.now()-current.requested_at<3000;
  const cooldown=()=>new Response(JSON.stringify({error:'请等待3秒后重试',code:'WRITE_COOLDOWN'}),{status:429,headers:{...headers,'Retry-After':'3'}});
  if(cooling(await legacyState(env,row)||row))return cooldown();
  const response=await setDesired(request,env,url,row,b,{...handlers,controllerCooldownMs:3000}),result=await response.json();
  const current=await get(env,row.id);
  if(response.status===409&&cooling(await legacyState(env,current)||current))return cooldown();
  return json({...result,device:await controllerDevice(await legacyState(env,current)||current,env)},response.status);
}
export const multiDeviceController=(request,env,url,handlers={})=>guarded(request,env,url,(r,e,u)=>controller(r,e,u,handlers));
