// Prepared for insertion into product2 only after the user's approval.
// No shared request state; no provider API keys, cookies or epaper permissions.
const AI_USAGE_HEADERS = {'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-store','X-Content-Type-Options':'nosniff','Referrer-Policy':'no-referrer'};
const AI_USAGE_SUMMARY = ['tokens','cost_usd','requests','cache_hit','input_tokens','output_tokens','cache_read_tokens','cache_write_tokens'];
function aiUsageResponse(body, status=200) {
  const text=typeof body==='string'?body:JSON.stringify(body);
  return new Response(text,{status,headers:{...AI_USAGE_HEADERS,'Content-Length':String(new TextEncoder().encode(text).length)}});
}
function aiUsageObject(value, keys) {
  return value && typeof value==='object' && !Array.isArray(value) && Object.keys(value).length===keys.length && keys.every(key=>Object.hasOwn(value,key));
}
function aiUsageNumber(value, integer=false, maximum=9e15) {
  return typeof value==='number' && Number.isFinite(value) && value>=0 && value<=maximum && (!integer||Number.isSafeInteger(value));
}
function aiUsageLabel(value) { return typeof value==='string' && value.length<=128 && !/[\x00-\x1f\x7f]/.test(value); }
function aiUsageSummary(value) {
  return aiUsageObject(value,AI_USAGE_SUMMARY) && AI_USAGE_SUMMARY.every(key=>aiUsageNumber(value[key],!['cost_usd','cache_hit'].includes(key))) && value.cache_hit<=1;
}
function aiUsageGroup(row) {
  return aiUsageObject(row,['name','requests','tokens','cost_usd'])&&aiUsageLabel(row.name)&&aiUsageNumber(row.requests,true)&&aiUsageNumber(row.tokens,true)&&aiUsageNumber(row.cost_usd);
}
function validateAiUsage(value, now) {
  const keys=['version','generated_at','range_start','available','today','yesterday','comparison','apps','models','providers','recent'];
  if(value&&Object.hasOwn(value,'model_ranges'))keys.push('model_ranges');
  if(!aiUsageObject(value,keys) || value.version!==1 || typeof value.available!=='boolean')return false;
  if(!aiUsageNumber(value.generated_at,true) || value.generated_at>now+30 || now-value.generated_at>60 || !aiUsageNumber(value.range_start,true) || value.range_start>value.generated_at)return false;
  if(!['today','yesterday','comparison'].every(key=>aiUsageSummary(value[key])))return false;
  if(!Array.isArray(value.apps)||value.apps.length>4||!value.apps.every(row=>aiUsageObject(row,['name',...AI_USAGE_SUMMARY])&&aiUsageLabel(row.name)&&aiUsageSummary(Object.fromEntries(AI_USAGE_SUMMARY.map(key=>[key,row[key]])))))return false;
  for(const name of ['models','providers']) {
    if(!Array.isArray(value[name])||value[name].length>5||!value[name].every(row=>aiUsageObject(row,['name','requests','tokens','cost_usd'])&&aiUsageLabel(row.name)&&aiUsageNumber(row.requests,true)&&aiUsageNumber(row.tokens,true)&&aiUsageNumber(row.cost_usd)))return false;
  }
  if(Object.hasOwn(value,'model_ranges') && (!aiUsageObject(value.model_ranges,['today','7d','30d','all']) || !Object.values(value.model_ranges).every(rows=>Array.isArray(rows)&&rows.every(aiUsageGroup)&&aiUsageNumber(rows.reduce((sum,r)=>sum+r.tokens,0),true))))return false;
  return Array.isArray(value.recent)&&value.recent.length<=8&&value.recent.every(row=>{
    if(!row||typeof row!=='object'||Array.isArray(row))return false;
    const keys=['ts','app','model','provider','tokens','cost_usd','status'];
    const extended=Object.hasOwn(row,'context_tokens');
    if(extended)keys.push('context_tokens','cache_hit','first_token_ms','output_tps');
    return aiUsageObject(row,keys)&&['app','model','provider'].every(key=>aiUsageLabel(row[key]))&&aiUsageNumber(row.ts,true)&&aiUsageNumber(row.tokens,true)&&aiUsageNumber(row.cost_usd)&&aiUsageNumber(row.status,true,599)&&(!extended || (aiUsageNumber(row.context_tokens,true)&&['cache_hit','first_token_ms','output_tps'].every(key=>row[key]===null||aiUsageNumber(row[key],false,key==='cache_hit'?1:9e15))));
  });
}
async function aiUsageAuthorized(request, expected) {
  const token=/^Bearer ([a-f0-9]{64})$/.exec(request.headers.get('Authorization')||'')?.[1]||'';
  const encoder=new TextEncoder();
  const [actualHash,expectedHash]=await Promise.all([crypto.subtle.digest('SHA-256',encoder.encode(token)),crypto.subtle.digest('SHA-256',encoder.encode(expected))]);
  return crypto.subtle.timingSafeEqual(actualHash,expectedHash);
}
async function readAiUsageBody(request) {
  if(!request.headers.get('Content-Type')?.startsWith('application/json')||Number(request.headers.get('Content-Length')||0)>262144)return null;
  const reader=request.body?.getReader();if(!reader)return null;
  const chunks=[];let size=0;
  try {
    for(;;){const {done,value}=await reader.read();if(done)break;size+=value.byteLength;if(size>262144){await reader.cancel();return null;}chunks.push(value);}
  }finally{reader.releaseLock();}
  const bytes=new Uint8Array(size);let offset=0;for(const value of chunks){bytes.set(value,offset);offset+=value.byteLength;}
  try{return JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes));}catch{return null;}
}
async function handleAiUsage(request,env) {
  const url=new URL(request.url);
  const models=/^\/api\/aiusage\/models\/(today|7d|30d|all)\/(0|[1-9][0-9]{0,5})$/.exec(url.pathname);
  if(url.pathname!=='/api/aiusage'&&!models)return aiUsageResponse({error:'not_found'},404);
  if(url.search)return aiUsageResponse({error:'query_not_allowed'},400);
  if(!['GET','POST'].includes(request.method))return aiUsageResponse({error:'method_not_allowed'},405);
  if(models&&request.method!=='GET')return aiUsageResponse({error:'method_not_allowed'},405);
  if(!env.AI_USAGE_DB||!/^([a-f0-9]{64})$/.test(env.AI_USAGE_READ_TOKEN||'')||!/^([a-f0-9]{64})$/.test(env.AI_USAGE_WRITE_TOKEN||''))return aiUsageResponse({error:'unavailable'},503);
  const writing=request.method==='POST';
  if(!await aiUsageAuthorized(request,writing?env.AI_USAGE_WRITE_TOKEN:env.AI_USAGE_READ_TOKEN))return aiUsageResponse({error:'unauthorized'},401);
  const limiter=writing?env.AI_USAGE_WRITE_LIMITER:env.AI_USAGE_READ_LIMITER;
  if(limiter&&!(await limiter.limit({key:writing?'cyd-write':'cyd-read'})).success)return aiUsageResponse({error:'rate_limited'},429);
  const now=Math.floor(Date.now()/1000);
  try {
    if(writing) {
      const body=await readAiUsageBody(request);
      if(!validateAiUsage(body,now))return aiUsageResponse({error:'invalid_snapshot'},400);
      const {model_ranges,...base}=body;
      if(new TextEncoder().encode(JSON.stringify(base)).length>8192)return aiUsageResponse({error:'base_too_large'},400);
      const statement=env.AI_USAGE_DB.prepare('INSERT INTO cyd_usage_snapshot(id,generated_at,uploaded_at,payload) VALUES(1,?,?,?) ON CONFLICT(id) DO UPDATE SET generated_at=excluded.generated_at,uploaded_at=excluded.uploaded_at,payload=excluded.payload WHERE excluded.generated_at>=cyd_usage_snapshot.generated_at').bind(body.generated_at,now,JSON.stringify(base));
      const result=model_ranges ? (await env.AI_USAGE_DB.batch([statement,env.AI_USAGE_DB.prepare('INSERT INTO cyd_usage_model_ranges(id,generated_at,uploaded_at,payload) SELECT 1,?,?,? WHERE ?>=(SELECT generated_at FROM cyd_usage_snapshot WHERE id=1) ON CONFLICT(id) DO UPDATE SET generated_at=excluded.generated_at,uploaded_at=excluded.uploaded_at,payload=excluded.payload WHERE excluded.generated_at>=cyd_usage_model_ranges.generated_at').bind(body.generated_at,now,JSON.stringify(model_ranges),body.generated_at)]))[0] : await statement.run();
      if(!result.meta.changes)return aiUsageResponse({error:'older_snapshot'},409);
      return aiUsageResponse({ok:true,generated_at:body.generated_at});
    }
    const row=await env.AI_USAGE_DB.prepare(models?'SELECT generated_at,payload FROM cyd_usage_model_ranges WHERE id=1':'SELECT generated_at,payload FROM cyd_usage_snapshot WHERE id=1').first();
    if(!row||now-row.generated_at>30)return aiUsageResponse({error:'stale'},503);
    const data=JSON.parse(row.payload);
    if(models){
      const rows=data[models[1]], offset=Number(models[2]);
      return aiUsageResponse({version:1,generated_at:row.generated_at,range:models[1],offset,total:rows.length,total_tokens:rows.reduce((sum,r)=>sum+r.tokens,0),rows:rows.slice(offset,offset+4)});
    }
    const {model_ranges,...base}=data;
    return aiUsageResponse(base);
  } catch {
    console.warn(JSON.stringify({event:'aiusage_failed',kind:'storage_or_body'}));
    return aiUsageResponse({error:'unavailable'},503);
  }
}
