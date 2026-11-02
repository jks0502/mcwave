// 400x300, row-major, 50 bytes/row, MSB first: 1=white, 0=black.
// Caller has already authenticated and scoped the md_devices row.
export const SCREEN_BYTES=15000;
const json=(error,status)=>Response.json({error},{status,headers:{'Cache-Control':'no-store'}});
export async function putScreen(request,env,row,modes){
  const hash=request.headers.get('X-Frame-Hash'),mode=request.headers.get('X-Frame-Mode');
  if(!/^[a-f0-9]{16}$/.test(hash||'')||!modes.includes(mode))return json('画面元数据无效',400);
  if(request.headers.get('Content-Type')!=='application/octet-stream'||request.headers.has('Content-Encoding'))return json('画面格式无效',400);
  const declared=request.headers.get('Content-Length');
  if(declared!==null&&declared!==String(SCREEN_BYTES))return json('画面长度必须为15000字节',400);
  const reader=request.body?.getReader();if(!reader)return json('画面为空',400);
  const frame=new Uint8Array(SCREEN_BYTES);let size=0;
  try{for(;;){const {done,value}=await reader.read();if(done)break;if(size+value.length>SCREEN_BYTES){await reader.cancel();return json('画面过大',400);}frame.set(value,size);size+=value.length;}}
  catch{return json('画面读取失败',400);}finally{reader.releaseLock();}
  if(size!==SCREEN_BYTES)return json('画面长度必须为15000字节',400);
  const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',frame)),b=>b.toString(16).padStart(2,'0')).join('').slice(0,16);
  if(digest!==hash)return json('画面校验失败',400);
  const old=await env.CONTROL_DB.prepare('SELECT hash FROM md_screens WHERE device_id=?').bind(row.id).first();
  if(old?.hash===hash)return new Response(null,{status:204,headers:{'Cache-Control':'no-store'}});
  const now=Date.now();
  const saved=await env.CONTROL_DB.prepare(`INSERT INTO md_screens(device_id,frame,hash,mode,updated_at) VALUES(?,?,?,?,?)
    ON CONFLICT(device_id) DO UPDATE SET frame=excluded.frame,hash=excluded.hash,mode=excluded.mode,updated_at=excluded.updated_at
    WHERE md_screens.updated_at<=? RETURNING hash`).bind(row.id,frame,hash,mode,now,now-60000).first();
  // A simultaneous identical upload may have won after the first read.
  const duplicate=!saved&&await env.CONTROL_DB.prepare('SELECT hash FROM md_screens WHERE device_id=?').bind(row.id).first();
  return saved||duplicate?.hash===hash?new Response(null,{status:204,headers:{'Cache-Control':'no-store'}}):json('屏幕画面每分钟最多更新一次',429);
}
export async function getScreen(request,env,row,{allowRanges=false}={}){
  if(request.method!=='GET')return json('不支持的操作',405);
  const saved=await env.CONTROL_DB.prepare('SELECT frame,hash,mode,updated_at FROM md_screens WHERE device_id=?').bind(row.id).first();
  if(!saved)return json('尚未收到屏幕画面',404);
  const headers={'Cache-Control':'private, no-cache','ETag':`"${saved.hash}"`,'X-Updated-At':String(saved.updated_at),'X-Frame-Mode':saved.mode,'X-Content-Type-Options':'nosniff'};
  if(allowRanges&&request.headers.has('If-Match')&&request.headers.get('If-Match')!==headers.ETag)return new Response(null,{status:412,headers});
  const tags=(request.headers.get('If-None-Match')||'').split(',').map(s=>s.trim().replace(/^W\//,''));
  if(tags.includes(headers.ETag)||tags.includes('*'))return new Response(null,{status:304,headers});
  const frame=new Uint8Array(saved.frame),binary={...headers,'Content-Type':'application/octet-stream','Content-Length':String(SCREEN_BYTES),'X-Image-Format':'mono1-400x300-msb-white1'};
  if(allowRanges&&request.headers.has('Range')){
    const match=/^bytes=(\d{1,5})-(\d{1,5})$/.exec(request.headers.get('Range'));
    const first=Number(match?.[1]),last=Number(match?.[2]);
    if(!match||first>last||last>=SCREEN_BYTES||last-first+1>3072)return new Response(null,{status:416,headers:{...headers,'Content-Range':`bytes */${SCREEN_BYTES}`}});
    return new Response(frame.subarray(first,last+1),{status:206,headers:{...binary,'Content-Length':String(last-first+1),'Content-Range':`bytes ${first}-${last}/${SCREEN_BYTES}`,'Accept-Ranges':'bytes'}});
  }
  return new Response(frame,{headers:binary});
}
