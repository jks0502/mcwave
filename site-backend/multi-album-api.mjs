import {commitTarget} from './device-target.mjs';
import {legacyBinding} from './legacy-device-bridge.mjs';
import {bitmapHash} from './album-store.mjs';
const ID=/^[a-f0-9]{64}$/,SIZE=15000,FORMAT='mono-400x300-msb-v1';
const headers={'Cache-Control':'no-store','Content-Type':'application/json; charset=utf-8','X-Content-Type-Options':'nosniff'};
const json=(o,status=200)=>new Response(JSON.stringify(o),{status,headers});
const fail=(error,status=400,code='INVALID_ALBUM')=>json({error,code},status);
const uint=n=>Number.isSafeInteger(n)&&n>=0&&n<=4294967295;
const keys=(o,k)=>o&&typeof o==='object'&&!Array.isArray(o)&&Object.keys(o).every(x=>k.includes(x));
export const multiAlbumEnabled=env=>env.MULTI_DEVICE_ENABLED==='true'&&env.MULTI_DEVICE_ALBUM_ENABLED==='true';
export const albumSupported=row=>['mcwave-hink-v1','edison-hink-s3-album-v1'].includes(row.profile_id)&&(!JSON.parse(row.report_json).supportedModes||JSON.parse(row.report_json).supportedModes.includes('album'));

async function readJson(request,limit=71680){
  if(!request.headers.get('Content-Type')?.startsWith('application/json')||Number(request.headers.get('Content-Length')||0)>limit)return null;
  const reader=request.body?.getReader();if(!reader)return null;const data=new Uint8Array(limit);let size=0;
  try{for(;;){const r=await reader.read();if(r.done)break;if(size+r.value.length>limit){await reader.cancel();return null;}data.set(r.value,size);size+=r.value.length;}const value=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(data.subarray(0,size)));return keys(value,Object.keys(value||{}))?value:null;}catch{return null;}finally{reader.releaseLock();}
}
async function state(env,row){
  let s=await env.CONTROL_DB.prepare('SELECT * FROM md_album_state WHERE device_id=?').bind(row.id).first();
  if(!s){await env.CONTROL_DB.prepare('INSERT OR IGNORE INTO md_album_state(device_id) VALUES(?)').bind(row.id).run();s=await env.CONTROL_DB.prepare('SELECT * FROM md_album_state WHERE device_id=?').bind(row.id).first();}
  return s;
}
const ids=async(env,id)=>(await env.CONTROL_DB.prepare('SELECT photo_id FROM md_album_items WHERE device_id=? ORDER BY position').bind(id).all()).results.map(r=>r.photo_id);
async function assetRevision(env){const s=await env.ALBUM_DB.prepare('SELECT revision FROM album_state WHERE id=1').first();if(!s)throw Error('asset catalog missing');return s.revision;}
// A device may only reference its own photos or the shared legacy ones (owner NULL), never another device's.
async function activeIds(env,list,deviceId){
  if(!list.length)return [];
  const rows=await env.ALBUM_DB.prepare('SELECT id FROM album_photos WHERE deleted_at IS NULL AND enabled=1 AND format_version=1 AND (owner IS NULL OR owner=?) AND id IN(SELECT value FROM json_each(?))').bind(deviceId,JSON.stringify(list)).all();
  const active=new Set(rows.results.map(r=>r.id));return list.filter(id=>active.has(id));
}
function desiredFor(row,s,selected,revision,activate){
  const old=JSON.parse(row.desired_json);
  return activate||old.mode==='album'?{mode:'album',page:0,photoId:selected,playback:s.playback,interval:s.interval_seconds,albumRevision:revision}:old;
}
async function save(env,row,s,list,config,{activate=false,assetRev=-1,replaceItems=false,cooldownMs=0}={}){
  const selected=list===null?config.selected_id:list.includes(config.selected_id)?config.selected_id:(list[0]||'');
  const desired=desiredFor(row,config,selected,s.revision+1,activate),db=env.CONTROL_DB;
  return commitTarget(env,row,row.revision,desired,{cooldownMs,albumRevision:s.revision,refresh:activate||JSON.parse(row.desired_json).mode==='album',edits:command=>{
    const guard='EXISTS(SELECT 1 FROM md_devices WHERE id=? AND command_id=?)';
    return [
      db.prepare(`UPDATE md_album_state SET revision=revision+1,playback=?,interval_seconds=?,selected_id=?,asset_revision=?,item_count=? WHERE device_id=? AND ${guard}`).bind(config.playback,config.interval_seconds,selected,assetRev,list?.length??s.item_count,row.id,row.id,command),
      ...(replaceItems?[db.prepare(`DELETE FROM md_album_items WHERE device_id=? AND ${guard}`).bind(row.id,row.id,command),
      db.prepare(`INSERT INTO md_album_items(device_id,photo_id,position) SELECT ?,value,CAST(key AS INTEGER) FROM json_each(?) WHERE ${guard}`).bind(row.id,JSON.stringify(list),row.id,command)]:[])
    ];
  }});
}
// Two D1 databases cannot commit atomically together. Resource reads always check
// the live photo catalog; this reconciliation removes revoked references on sync.
export async function reconcileDeviceAlbum(env,row){
  if(!multiAlbumEnabled(env)||!albumSupported(row)||!row.enabled)return row;
  if(!env.ALBUM_DB)throw Error('album storage missing');
  const s=await state(env,row),version=await assetRevision(env);if(s.asset_revision===version)return row;
  const list=await ids(env,row.id),active=await activeIds(env,list,row.id);
  if(active.length===list.length){await env.CONTROL_DB.prepare('UPDATE md_album_state SET asset_revision=? WHERE device_id=? AND revision=?').bind(version,row.id,s.revision).run();return row;}
  const updated=await save(env,row,s,active,s,{assetRev:version,replaceItems:true});
  if(!updated)throw Error('playlist changed during cleanup');return updated;
}
function ready(env,row){return multiAlbumEnabled(env)&&env.ALBUM_DB&&albumSupported(row);}
async function snapshot(env,row,summary=false){
  await state(env,row);
  const joined=env.CONTROL_DB.prepare(`SELECT d.*,s.revision AS album_revision,s.playback AS album_playback,s.interval_seconds AS album_interval,s.selected_id AS album_selected,s.item_count AS album_count FROM md_devices d JOIN md_album_state s ON s.device_id=d.id WHERE d.id=? AND d.space_id='personal'`).bind(row.id);
  let list=null;
  if(summary)row=await joined.first();
  else{const result=await env.CONTROL_DB.batch([joined,env.CONTROL_DB.prepare('SELECT photo_id FROM md_album_items WHERE device_id=? ORDER BY position').bind(row.id)]);row=result[0].results[0];list=result[1].results.map(item=>item.photo_id);}
  if(!row)throw Error('device snapshot missing');
  const s={revision:row.album_revision,playback:row.album_playback,interval_seconds:row.album_interval,selected_id:row.album_selected,item_count:row.album_count};
  const report=JSON.parse(row.report_json);
  return {display:{mode:report.mode||'',photoId:report.photoId||'',online:!!row.enabled&&row.last_seen>Date.now()-90000,pending:row.applied_revision<row.revision||!row.session_id||row.confirmed_session!==row.session_id,error:report.error||''},enabled:!!row.enabled,deviceId:row.id,revision:row.revision,albumRevision:s.revision,playback:s.playback,interval:s.interval_seconds,photoId:s.selected_id,...(list?{photoIds:list}:{}),total:s.item_count,empty:!s.item_count,format:FORMAT,width:400,height:300,bytes:SIZE,cacheLimit:20,cacheByteBudget:20*SIZE};
}
export async function manageDeviceAlbum(request,env,url,row){
  if(!multiAlbumEnabled(env))return fail('未找到',404);
  if(!albumSupported(row))return fail('设备尚不支持相册',400,'UNSUPPORTED');
  if(await legacyBinding(env,row.id))return fail('当前设备使用兼容相册，请从原相册入口管理',409,'LEGACY_TRANSPORT');
  if(!env.ALBUM_DB)return fail('相册尚未配置',503);
  row=await reconcileDeviceAlbum(env,row);
  if(request.method==='GET')return json(await snapshot(env,row,url.searchParams.get('summary')==='1'));
  if(request.method!=='POST')return fail('不支持的操作',405);
  if(!row.enabled)return fail('设备已停用',409);
  const b=await readJson(request);
  if(!keys(b,['baseRevision','baseAlbumRevision','action','photoIds','photoId','beforeId','playback','interval'])||!uint(b.baseRevision)||!uint(b.baseAlbumRevision))return fail('无效播放设置');
  const s=await state(env,row);if(b.baseRevision!==row.revision||b.baseAlbumRevision!==s.revision)return fail('相册已变化，请重新选择',409,'REVISION_CONFLICT');
  const actionKeys={replace:['photoIds'],move:['photoId','beforeId'],settings:['photoId','playback','interval']};
  if(!Object.hasOwn(actionKeys,b.action)||!keys(b,['baseRevision','baseAlbumRevision','action',...actionKeys[b.action]]))return fail('Invalid operation fields');
  let list=await ids(env,row.id),config={...s};
  if(b.action==='replace'){
    if(!Array.isArray(b.photoIds)||b.photoIds.length>1000||new Set(b.photoIds).size!==b.photoIds.length||b.photoIds.some(id=>typeof id!=='string'||!ID.test(id)))return fail('照片清单无效');list=b.photoIds;
    if((await activeIds(env,list,row.id)).length!==list.length)return fail('照片已移除或格式不支持',409,'ASSET_UNAVAILABLE');
  }else if(b.action==='move'){
    if(!ID.test(b.photoId||'')||!list.includes(b.photoId)||!(b.beforeId===null||ID.test(b.beforeId||'')&&list.includes(b.beforeId))||b.beforeId===b.photoId)return fail('排序位置无效');
    list=list.filter(id=>id!==b.photoId);list.splice(b.beforeId===null?list.length:list.indexOf(b.beforeId),0,b.photoId);
  }else if(b.action!=='settings')return fail('操作无效');
  if(b.playback!==undefined){if(!['sequence','random','fixed'].includes(b.playback))return fail('播放模式无效');config.playback=b.playback;}
  if(b.interval!==undefined){if(![60,300,600].includes(b.interval))return fail('刷新间隔无效');config.interval_seconds=b.interval;}
  if(b.action==='settings'&&b.photoId!==undefined){if(!list.includes(b.photoId))return fail('照片不在本设备列表',409);config.selected_id=b.photoId;}
  const updated=await save(env,row,s,list,config,{activate:b.action==='settings',assetRev:await assetRevision(env),replaceItems:b.action!=='settings'});
  return updated?json(await snapshot(env,updated)):fail('其他人已更新，请重新选择',409,'REVISION_CONFLICT');
}
// Read-only: which device playlists would lose this photo if it is moved to the
// shared recycle bin. The legacy bridge plays every enabled shared photo.
export async function photoUsage(env,photo){
  if(!multiAlbumEnabled(env))return fail('未找到',404);
  if(!ID.test(photo))return fail('照片无效');
  const rows=(await env.CONTROL_DB.prepare(`SELECT d.id,d.name FROM md_album_items i JOIN md_devices d ON d.id=i.device_id WHERE i.photo_id=? AND d.space_id='personal' ORDER BY d.created_at,d.id`).bind(photo).all()).results;
  const devices=rows.map(r=>({id:r.id,name:r.name})),bridge=await legacyBinding(env);
  if(bridge&&env.ALBUM_DB&&!devices.some(d=>d.id===bridge.id)&&await env.ALBUM_DB.prepare('SELECT 1 FROM album_photos WHERE id=? AND deleted_at IS NULL AND enabled=1 AND owner IS NULL').bind(photo).first())devices.push({id:bridge.id,name:bridge.name,legacy:true});
  return json({photoId:photo,devices});
}
export async function activateDeviceAlbum(env,row,baseRevision,{cooldownMs=0}={}){
  if(!ready(env,row))return fail('相册尚未配置',503);
  row=await reconcileDeviceAlbum(env,row);if(row.revision!==baseRevision)return fail('状态已变化，请重试',409,'REVISION_CONFLICT');
  const s=await state(env,row);if(!s.item_count)return fail('请先添加照片',409,'EMPTY_ALBUM');
  const updated=await save(env,row,s,null,s,{activate:true,assetRev:await assetRevision(env),cooldownMs});
  return updated?json({saved:true}):fail('状态已变化，请重试',409,'REVISION_CONFLICT');
}
export async function deviceAlbumResource(request,env,url,row){
  if(!multiAlbumEnabled(env))return fail('未找到',404);
  if(!ready(env,row))return fail('此设备不支持相册',400,'UNSUPPORTED');
  if(url.searchParams.has('format')&&url.searchParams.get('format')!==FORMAT)return fail('Unsupported format',400,'UNSUPPORTED_FORMAT');
  const path=url.pathname.slice('/api/v2/device/album'.length);
  if(path==='/manifest'&&request.method==='POST'){
    const b=await readJson(request,2048);if(!keys(b,['knownRevision','cachedIds'])||!uint(b.knownRevision)||!Array.isArray(b.cachedIds)||b.cachedIds.length>20||new Set(b.cachedIds).size!==b.cachedIds.length||b.cachedIds.some(id=>typeof id!=='string'||!ID.test(id)))return fail('缓存清单无效');
    row=await reconcileDeviceAlbum(env,row);const result=await env.CONTROL_DB.batch([env.CONTROL_DB.prepare('SELECT * FROM md_album_state WHERE device_id=?').bind(row.id),env.CONTROL_DB.prepare('SELECT photo_id FROM md_album_items WHERE device_id=? AND photo_id IN(SELECT value FROM json_each(?)) ORDER BY position').bind(row.id,JSON.stringify(b.cachedIds))]);
    const s=result[0].results[0],members=result[1].results.map(x=>x.photo_id);
    return json({albumRevision:s.revision,unchanged:s.revision===b.knownRevision,validCachedIds:members,removeCachedIds:b.cachedIds.filter(id=>!members.includes(id)),photoId:s.selected_id,empty:!s.selected_id,format:FORMAT,bytes:SIZE,cacheLimit:20,cacheByteBudget:300000});
  }
  if(path==='/next'&&request.method==='POST'){
    const b=await readJson(request,2048);if(!keys(b,['baseRevision','baseAlbumRevision','currentId','sessionId'])||!uint(b.baseRevision)||!uint(b.baseAlbumRevision)||!ID.test(b.currentId||'')||b.sessionId!==row.session_id||!row.session_id)return fail('下一张请求无效');
    row=await reconcileDeviceAlbum(env,row);
    if(JSON.parse(row.desired_json).mode!=='album'||row.applied_revision<row.revision||row.confirmed_session!==row.session_id)return fail('Render pending',409,'WAIT_RENDER');
    const s=await state(env,row);
    if(b.baseRevision!==row.revision||b.baseAlbumRevision!==s.revision||b.currentId!==s.selected_id)return json({error:'播放位置已变化',code:'REVISION_CONFLICT',revision:row.revision,desired:JSON.parse(row.desired_json)},409);
    if(s.playback==='fixed')return fail('固定模式不自动换图',409,'FIXED_PLAYBACK');
    if(!s.item_count)return fail('Empty playlist; retain frame',409,'EMPTY_ALBUM');
    const current=await env.CONTROL_DB.prepare('SELECT position FROM md_album_items WHERE device_id=? AND photo_id=?').bind(row.id,s.selected_id).first();
    if(!current)throw Error('playlist cursor missing');
    let position=(current.position+1)%s.item_count;
    if(s.playback==='random'&&s.item_count>1){const count=s.item_count-1,v=new Uint32Array(1),bound=4294967296-4294967296%count;do{crypto.getRandomValues(v);}while(v[0]>=bound);position=v[0]%count;if(position>=current.position)position++;}
    const next=await env.CONTROL_DB.prepare('SELECT photo_id FROM md_album_items WHERE device_id=? AND position=?').bind(row.id,position).first();
    if(!next)throw Error('playlist position missing');
    const updated=await save(env,row,s,null,{...s,selected_id:next.photo_id},{activate:true,assetRev:await assetRevision(env)});
    return updated?json({revision:updated.revision,desired:JSON.parse(updated.desired_json)}):fail('位置已变化，请重读',409,'REVISION_CONFLICT');
  }
  const image=/^\/images\/([a-f0-9]{64})$/.exec(path)?.[1];
  if(image&&['GET','HEAD'].includes(request.method)){
    const member=await env.CONTROL_DB.prepare('SELECT photo_id FROM md_album_items WHERE device_id=? AND photo_id=?').bind(row.id,image).first();
    if(!member)return fail('已从本设备列表移除',410,'ASSET_REVOKED');
    const photo=await env.ALBUM_DB.prepare('SELECT bitmap,sha256 FROM album_photos WHERE id=? AND deleted_at IS NULL AND enabled=1 AND format_version=1 AND (owner IS NULL OR owner=?)').bind(image,row.id).first();
    if(!photo)return fail('照片已移除',410,'ASSET_REVOKED');
    const bytes=Uint8Array.from(photo.bitmap);if(bytes.length!==SIZE||await bitmapHash(bytes)!==image||photo.sha256!==image)return fail('照片校验失败，保留原画面',503,'ASSET_CORRUPT');
    return new Response(request.method==='HEAD'?null:bytes,{headers:{'Cache-Control':'no-store','Content-Type':'application/octet-stream','Content-Length':String(SIZE),'X-Image-Sha256':image,'X-Image-Format':FORMAT}});
  }
  return fail('未找到',404);
}
