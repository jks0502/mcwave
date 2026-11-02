// mcwave 6.0.0 two-person mood. Each person's browser draws their own half
// (200x300, 1 bit per pixel, 1=white, MSB first: 7500 bytes); the Worker only
// validates and stores it. Side 0 is jks (left), side 1 is momosaysay (right).
export const MOOD_NAMES=['jks','momosaysay'];
export const MOODS=['开心','想你','累了','难过','生气','平静','忙碌','困了'];
export const MOOD_HALF_BYTES=7500;
const NOTE_MAX=40,BODY_MAX=16384;
const headers={'Cache-Control':'no-store','Content-Type':'application/json; charset=utf-8','X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer'};
const json=(body,status=200)=>new Response(JSON.stringify(body),{status,headers});
const base64=bytes=>{let s='';for(const b of bytes)s+=String.fromCharCode(b);return btoa(s);};

async function readBody(request){
  if(!request.headers.get('Content-Type')?.startsWith('application/json')||Number(request.headers.get('Content-Length')||0)>BODY_MAX)return null;
  const reader=request.body?.getReader();if(!reader)return null;
  const buffer=new Uint8Array(BODY_MAX);let size=0;
  try{
    for(;;){const r=await reader.read();if(r.done)break;if(size+r.value.length>BODY_MAX){await reader.cancel();return null;}buffer.set(r.value,size);size+=r.value.length;}
    return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(buffer.subarray(0,size)));
  }catch{return null;}finally{reader.releaseLock();}
}
function decodeHalf(text){
  if(typeof text!=='string'||text.length!==10000)return null;
  try{const bytes=Uint8Array.from(atob(text),c=>c.charCodeAt(0));return bytes.length===MOOD_HALF_BYTES?bytes:null;}catch{return null;}
}
const validNote=note=>typeof note==='string'&&Array.from(note).length<=NOTE_MAX&&!/[\u0000-\u001f\u007f]/.test(note);

// Versions for the device's sync reply; [0,0] when nothing is stored yet.
export async function moodVersions(env){
  const rows=await env.CONTROL_DB.prepare('SELECT side,version FROM md_mood').all();
  const out=[0,0];for(const r of rows.results||[])if(r.side===0||r.side===1)out[r.side]=r.version;
  return out;
}

// GET/POST /api/epaper/mood for a signed-in person; `side` comes from the session.
export async function moodWeb(request,env,side){
  if(!env.CONTROL_DB)return json({error:'服务尚未配置'},503);
  try{
    if(request.method==='GET'){
      const rows=await env.CONTROL_DB.prepare('SELECT side,mood,note,bitmap,version,updated_at FROM md_mood').all();
      const sides=MOOD_NAMES.map((name,i)=>{
        const r=(rows.results||[]).find(x=>x.side===i);
        return {name,mood:r?.mood||'',note:r?.note||'',version:r?.version||0,updatedAt:r?.updated_at||0,image:r?base64(new Uint8Array(r.bitmap)):null};
      });
      return json({me:side,moods:MOODS,noteMax:NOTE_MAX,sides});
    }
    if(request.method==='POST'){
      const b=await readBody(request);const bitmap=b&&decodeHalf(b.image);
      if(!b||!MOODS.includes(b.mood)||!validNote(b.note??'')||!bitmap)return json({error:'心情内容无效'},400);
      const now=Date.now();
      // One counter across both sides keeps versions increasing even if a row is reset.
      const row=await env.CONTROL_DB.prepare(`INSERT INTO md_mood(side,mood,note,bitmap,version,updated_at)
        VALUES(?1,?2,?3,?4,(SELECT COALESCE(MAX(version),0)+1 FROM md_mood),?5)
        ON CONFLICT(side) DO UPDATE SET mood=excluded.mood,note=excluded.note,bitmap=excluded.bitmap,version=excluded.version,updated_at=excluded.updated_at
        RETURNING version,updated_at`).bind(side,b.mood,(b.note??'').trim(),bitmap,now).first();
      return json({version:row.version,updatedAt:row.updated_at});
    }
    return json({error:'Method not allowed'},405);
  }catch{
    console.error(JSON.stringify({event:'mood_database_failed'}));
    return json({error:'心情服务暂不可用，请稍后重试'},503);
  }
}

// GET /api/device/mood/<side>: the stored half for the (already authorized) device.
export async function moodDevice(env,side){
  const row=await env.CONTROL_DB.prepare('SELECT bitmap,version FROM md_mood WHERE side=?').bind(side).first();
  if(!row)return json({error:'Not found'},404);
  return new Response(new Uint8Array(row.bitmap),{headers:{'Cache-Control':'no-store','Content-Type':'application/octet-stream','X-Mood-Version':String(row.version)}});
}
