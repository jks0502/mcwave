import {authorizeLegacyDevice} from './legacy-device-bridge.mjs';
import {readBitmap,bitmapHash} from './album-store.mjs';
const SIZE=15000, MAX=1000, ID=/^[a-f0-9]{64}$/, DEVICE_ID=/^[a-f0-9-]{36}$/;
const headers={'Cache-Control':'no-store','X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer'};
const json=(body,status=200,extra={})=>Response.json(body,{status,headers:{...headers,...extra}});
const fields='id,name,sha256,settings,format_version,sort_order,enabled,created_at,deleted_at';
// Select one ID without fetching BLOBs or a thousand-photo playlist into RAM.
// owner: null is the legacy (mcwave) album, otherwise the device ID that owns the photos.
export async function pickAlbumPhoto(env,current='',direction=1,random=false,owner=null) {
  if(!env.ALBUM_DB)return null;
  const db=env.ALBUM_DB,where='deleted_at IS NULL AND enabled=1 AND owner IS ?';
  if(random) {
    const {count}=await db.prepare(`SELECT COUNT(*) count FROM album_photos WHERE ${where} AND id<>?`).bind(owner,current).first();
    if(count) {
      const sample=new Uint32Array(1), bound=4294967296-4294967296%count;
      do{crypto.getRandomValues(sample);}while(sample[0]>=bound);
      return db.prepare(`SELECT id FROM album_photos WHERE ${where} AND id<>? ORDER BY sort_order,id LIMIT 1 OFFSET ?`).bind(owner,current,sample[0]%count).first();
    }
  }
  const row=await db.prepare(`SELECT sort_order FROM album_photos WHERE ${where} AND id=?`).bind(owner,current).first();
  if(row) {
    const next=await db.prepare(`SELECT id FROM album_photos WHERE ${where} AND sort_order${direction<0?'<':'>'}? ORDER BY sort_order ${direction<0?'DESC':'ASC'},id LIMIT 1`).bind(owner,row.sort_order).first();
    if(next)return next;
  }
  return db.prepare(`SELECT id FROM album_photos WHERE ${where} ORDER BY sort_order ${direction<0?'DESC':'ASC'},id LIMIT 1`).bind(owner).first();
}
const state=db=>db.prepare('SELECT * FROM album_state WHERE id=1').first();
const advance=db=>db.prepare('UPDATE album_state SET revision=revision+1 WHERE id=1 AND changes()>0');
const info=s=>({revision:s.revision,currentId:s.current_id,active:s.active_count,trash:s.stored_count-s.active_count,stored:s.stored_count,limit:MAX,pixelBytes:s.stored_count*SIZE});
// Counts shown to a device's page cover only that owner's photos (the revision stays shared).
const scoped=async(db,s,owner)=>{const c=await db.prepare('SELECT COUNT(*) AS stored,COALESCE(SUM(deleted_at IS NULL),0) AS active FROM album_photos WHERE owner IS ?').bind(owner).first();return info({...s,stored_count:c.stored,active_count:c.active});};
function metadata(row) {const {owner,...rest}=row;return {...rest,settings:JSON.parse(row.settings),width:400,height:300,bytes:SIZE};}
export function validSettings(s) {
  return s && Object.keys(s).length===10 && s.formatVersion===1 &&
    [0,90,180,270].includes(s.rotation) && ['contain','cover'].includes(s.fit) &&
    Number.isFinite(s.zoom)&&s.zoom>=1&&s.zoom<=3 &&
    ['x','y'].every(k=>Number.isFinite(s[k])&&Math.abs(s[k])<=1) &&
    ['brightness','contrast'].every(k=>Number.isInteger(s[k])&&Math.abs(s[k])<=100) &&
    ['diffusion','bayer','threshold'].includes(s.mode) && Number.isInteger(s.threshold)&&s.threshold>=32&&s.threshold<=224;
}
async function bodyJson(request) {
  if(request.headers.get('Content-Type')?.split(';')[0]!=='application/json' || Number(request.headers.get('Content-Length')||0)>512)return null;
  const reader=request.body?.getReader();if(!reader)return null;
  const buffer=new Uint8Array(512);let size=0;
  try {for(;;){const {value,done}=await reader.read();if(done)break;if(size+value.length>512){await reader.cancel();return null;}buffer.set(value,size);size+=value.length;}
    return JSON.parse(new TextDecoder().decode(buffer.subarray(0,size)));
  }catch{return null;}finally{reader.releaseLock();}
}
export function albumEnabled(env,url) {
  return env.ALBUM_ENABLED==='true' || (env.LOCAL_PREVIEW==='true'&&['localhost','127.0.0.1','[::1]'].includes(url.hostname));
}
export async function albumDevice(request,env,url) {
  if(!albumEnabled(env,url))return json({error:'Not found'},404);
  if(url.protocol!=='https:'&&!['localhost','127.0.0.1','[::1]'].includes(url.hostname))return json({error:'HTTPS required'},400);
  if(!['GET','HEAD'].includes(request.method))return json({error:'设备仅有读取权限。'},405);
  if(env.MULTI_DEVICE_ENABLED!=='true'&&(!env.DEVICE_TOKEN || env.DEVICE_TOKEN.length<32))return json({error:'设备访问未配置。'},503);
  try {if(!await authorizeLegacyDevice(request,env))return json({error:'Unauthorized'},401);}
  catch {return json({error:'Service unavailable'},503);}
  return albumApi(request,env,url.pathname.slice('/api/device/album'.length)||'/','device');
}
export async function albumApi(request,env,path,access='manage') {
  if(!env.ALBUM_DB || !env.ALBUM_LIMITER || !env.ALBUM_WRITE_LIMITER)return json({error:'相册存储尚未配置。'},503);
  const write=request.method==='POST';
  if(!['GET','HEAD','POST'].includes(request.method)||access==='device'&&write)return json({error:'Method not allowed'},405);
  if(access==='device' && !(path==='/next'||path==='/playlist'||/^\/images\/[a-f0-9]{64}$/.test(path)))return json({error:'设备仅可读取播放清单和点阵。'},403);
  const limiter=write?env.ALBUM_WRITE_LIMITER:env.ALBUM_LIMITER;
  try {
    if(!(await limiter.limit({key:`album:${access}:${request.headers.get('CF-Connecting-IP')||'local'}`})).success)return json({error:'操作较频繁，请一分钟后重试。'},429,{'Retry-After':'60'});
  }catch{return json({error:'相册访问暂时不可用，请稍后重试。'},503);}
  const db=env.ALBUM_DB,url=new URL(request.url);
  // Devices on the legacy protocol always use the legacy album; a device's own pages pass ?device=<ID>.
  const owner=access==='manage'?url.searchParams.get('device'):null;
  if(owner!==null&&!DEVICE_ID.test(owner))return json({error:'无效设备'},400);
  try {
    if(!write && path==='/next') {
      const current=url.searchParams.get('current')||'';
      if(current&&!ID.test(current))return json({error:'无效照片ID'},400);
      const row=await pickAlbumPhoto(env,current,url.searchParams.get('direction')==='-1'?-1:1,url.searchParams.get('random')==='1',owner);
      return row?json(row):json({error:'播放清单为空'},404);
    }
    if(!write && ['/', '/playlist'].includes(path)) {
      const rawPage=url.searchParams.get('page')||'0';if(!/^\d{1,3}$/.test(rawPage)||Number(rawPage)>83)return json({error:'无效页码。'},400);
      const page=Number(rawPage), trash=url.searchParams.get('trash')==='1'&&path==='/', playlist=path==='/playlist';
      const where=(trash?'deleted_at IS NOT NULL':`deleted_at IS NULL${playlist?' AND enabled=1':''}`)+' AND owner IS ?';
      const [s,rows,total]=await db.batch([
        db.prepare('SELECT * FROM album_state WHERE id=1'),
        db.prepare(`SELECT ${access==='device'?'id,sha256,format_version':fields} FROM album_photos WHERE ${where} ORDER BY sort_order,id LIMIT 12 OFFSET ?`).bind(owner,page*12),
        db.prepare(`SELECT COUNT(*) AS count FROM album_photos WHERE ${where}`).bind(owner)
      ]);
      const current=s.results[0];if(!current)return json({error:'相册尚未初始化。'},503);
      const count=total.results[0].count;
      return json({...(access==='manage'?await scoped(db,current,owner):{revision:current.revision,currentId:current.current_id}),page,total:count,pages:Math.max(1,Math.ceil(count/12)),
        items:access==='device'?rows.results.map(r=>({...r,bytes:SIZE})):rows.results.map(metadata)});
    }
    const metadataId=/^\/photos\/([a-f0-9]{64})$/.exec(path)?.[1];
    if(metadataId&&!write) {
      const row=await db.prepare(`SELECT ${fields} FROM album_photos WHERE id=? AND deleted_at IS NULL AND owner IS ?`).bind(metadataId,owner).first();
      if(!row)return json({error:'照片不存在或已移入回收站。'},404);
      const position=await db.prepare('SELECT COUNT(*) AS count FROM album_photos WHERE deleted_at IS NULL AND owner IS ? AND (sort_order<? OR (sort_order=? AND id<?))').bind(owner,row.sort_order,row.sort_order,row.id).first();
      return json({...metadata(row),position:position.count,...await scoped(db,await state(db),owner)});
    }
    const imageId=/^\/images\/([a-f0-9]{64})$/.exec(path)?.[1];
    if(imageId&&!write) {
      const row=(await db.prepare(`SELECT bitmap,sha256,name FROM album_photos WHERE id=? AND owner IS ?${access==='device'?' AND deleted_at IS NULL AND enabled=1':''}`).bind(imageId,owner).all()).results[0];
      if(!row)return json({error:'照片不存在或不在播放清单。'},404);
      const bytes=Uint8Array.from(row.bitmap);if(bytes.length!==SIZE)return json({error:'点阵数据不完整。'},503);
      return new Response(request.method==='HEAD'?null:bytes,{headers:{...headers,'Content-Type':'application/octet-stream','Content-Length':String(SIZE),'X-Image-Sha256':row.sha256,'X-Photo-Name':encodeURIComponent(row.name),'X-Image-Format':'mono-400x300-msb-v1'}});
    }
    if(path==='/'&&write) {
      let name,settings;
      try {const raw=request.headers.get('X-Photo-Settings')||'';if(raw.length>2048)throw new Error();
        name=decodeURIComponent(request.headers.get('X-Photo-Name')||'');settings=JSON.parse(decodeURIComponent(raw));
      }catch{return json({error:'名称或处理参数格式错误。'},400);}
      if(!name.trim()||name.length>80||/[\u0000-\u001f]/.test(name)||!validSettings(settings))return json({error:'照片名称或处理规格不符合要求。'},400);
      const bytes=await readBitmap(request);if(!bytes)return json({error:'点阵必须为400×300、15000字节。'},400);
      const hash=await bitmapHash(bytes), now=Date.now();
      const [result]=await db.batch([
        db.prepare(`INSERT INTO album_photos(id,name,bitmap,sha256,settings,sort_order,created_at,owner)
          SELECT ?,?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM album_photos),0),?,? WHERE (SELECT stored_count FROM album_state WHERE id=1)<1000
          ON CONFLICT(id) DO NOTHING RETURNING id`).bind(hash,name.trim(),bytes,hash,JSON.stringify(settings),now,owner),
        advance(db)
      ]);
      const existing=await db.prepare(`SELECT ${fields},owner FROM album_photos WHERE id=?`).bind(hash).first();
      if(!existing)return json({error:'相册已达1000张上限（包含回收站），请先备份并整理照片。'},409);
      // Photo IDs are content hashes, so one image can live in one device's album only.
      if((existing.owner??null)!==owner)return json({error:'这张图片已在另一台设备的相册里，请先在那里删除后再上传。'},409);
      const duplicate=!result.results.length;
      return json({...metadata(existing),...await scoped(db,await state(db),owner),duplicate,archived:existing.deleted_at!==null},duplicate?200:201);
    }
    if(path==='/playlist'&&write) {
      const body=await bodyJson(request);
      if(!body||!Number.isSafeInteger(body.baseRevision)||body.baseRevision<0 || !(body.currentId===null||typeof body.currentId==='string'&&ID.test(body.currentId)))return json({error:'无效播放清单指令。'},400);
      const result=await db.prepare(`UPDATE album_state SET current_id=?,revision=revision+1 WHERE id=1 AND revision=?
        AND (? IS NULL OR EXISTS(SELECT 1 FROM album_photos WHERE id=? AND deleted_at IS NULL AND enabled=1 AND owner IS ?)) RETURNING *`)
        .bind(body.currentId,body.baseRevision,body.currentId,body.currentId,owner).first();
      return result?json(await scoped(db,result,owner)):json({error:'相册已变化或照片已移除，请重新加载。'},409);
    }
    const match=/^\/photos\/([a-f0-9]{64})\/(delete|restore|purge|move|enabled|rename)$/.exec(path);
    if(match&&write) {
      const [,id,action]=match,body=await bodyJson(request);
      if(!body||!Number.isSafeInteger(body.baseRevision)||body.baseRevision<0)return json({error:'无效相册版本。'},400);
      const row=await db.prepare('SELECT sort_order,deleted_at,enabled FROM album_photos WHERE id=? AND owner IS ?').bind(id,owner).first();
      if(!row)return json({error:'照片不存在。'},404);
      let statement;
      const guard='(SELECT revision FROM album_state WHERE id=1)=?';
      if(action==='delete')statement=db.prepare(`UPDATE album_photos SET deleted_at=? WHERE id=? AND deleted_at IS NULL AND ${guard} RETURNING id`).bind(Date.now(),id,body.baseRevision);
      if(action==='restore')statement=db.prepare(`UPDATE album_photos SET deleted_at=NULL WHERE id=? AND deleted_at IS NOT NULL AND ${guard} RETURNING id`).bind(id,body.baseRevision);
      if(action==='purge') {
        if(body.confirm!==id)return json({error:'请明确确认永久删除。'},400);
        statement=db.prepare(`DELETE FROM album_photos WHERE id=? AND deleted_at IS NOT NULL AND ${guard} RETURNING id`).bind(id,body.baseRevision);
      }
      if(action==='enabled') {
        if(typeof body.enabled!=='boolean')return json({error:'无效播放设置。'},400);
        statement=db.prepare(`UPDATE album_photos SET enabled=? WHERE id=? AND deleted_at IS NULL AND ${guard} RETURNING id`).bind(Number(body.enabled),id,body.baseRevision);
      }
      if(action==='rename') {
        if(typeof body.name!=='string'||!body.name.trim()||body.name.length>80||/[\u0000-\u001f]/.test(body.name))return json({error:'无效照片名称。'},400);
        statement=db.prepare(`UPDATE album_photos SET name=? WHERE id=? AND deleted_at IS NULL AND ${guard} RETURNING id`).bind(body.name.trim(),id,body.baseRevision);
      }
      if(action==='move') {
        if(![-1,1].includes(body.direction)||row.deleted_at!==null||!['all','playlist'].includes(body.scope||'all')||(body.scope==='playlist'&&!row.enabled))return json({error:'无效排序。'},400);
        const previous=body.direction===-1;
        const neighbor=await db.prepare(`SELECT id,sort_order FROM album_photos WHERE deleted_at IS NULL AND owner IS ? ${body.scope==='playlist'?'AND enabled=1':''} AND sort_order${previous?'<':'>'}? ORDER BY sort_order ${previous?'DESC':'ASC'} LIMIT 1`).bind(owner,row.sort_order).first();
        if(!neighbor)return json({error:'照片已在这一方向的边界。'},409);
        statement=db.prepare(`UPDATE album_photos SET sort_order=CASE id WHEN ? THEN ? ELSE ? END WHERE id IN (?,?) AND deleted_at IS NULL AND ${guard} RETURNING id`)
          .bind(id,neighbor.sort_order,row.sort_order,id,neighbor.id,body.baseRevision);
      }
      const [result]=await db.batch([statement,advance(db)]);
      return result.results.length?json(await scoped(db,await state(db),owner)):json({error:'相册状态已变化，请重新加载后重试。'},409);
    }
    return json({error:'Not found'},404);
  }catch(error){console.error(JSON.stringify({operation:'album-api',errorType:error.name}));return json({error:'相册暂时不可用，已有照片仍保留。'},503);}
}
