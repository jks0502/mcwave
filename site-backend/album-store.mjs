const SIZE = 15000;
const headers = {'Cache-Control':'no-store', 'X-Content-Type-Options':'nosniff', 'Referrer-Policy':'no-referrer'};
const json = (body,status=200) => Response.json(body,{status,headers});
export async function readBitmap(request) {
  if (request.headers.get('Content-Type')?.split(';')[0] !== 'application/octet-stream' ||
      Number(request.headers.get('Content-Length') || 0) > SIZE) return null;
  const reader = request.body?.getReader();
  if (!reader) return null;
  const bytes = new Uint8Array(SIZE);
  let offset = 0;
  try {
    for (;;) {
      const {done,value} = await reader.read();
      if (done) break;
      if (offset + value.length > SIZE) { await reader.cancel(); return null; }
      bytes.set(value,offset); offset += value.length;
    }
    return offset === SIZE ? bytes : null;
  } catch { return null; }
  finally { reader.releaseLock(); }
}
export async function bitmapHash(bytes) {
  return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),b=>b.toString(16).padStart(2,'0')).join('');
}
// Step 2 proof only: at most 20 test images. This is not the full album API.
export async function albumProof(request,db,path) {
  if (!db) return json({error:'测试存储尚未配置。'},503);
  try {
    if (path === '/' && request.method === 'POST') {
      let name,settings;
      try {
        name = decodeURIComponent(request.headers.get('X-Photo-Name') || '未命名');
        const raw = request.headers.get('X-Photo-Settings') || '{}';
        if (raw.length > 2048) return json({error:'处理参数过长。'},400);
        settings = JSON.parse(decodeURIComponent(raw));
        if (!settings || typeof settings !== 'object' || Array.isArray(settings)) throw new Error();
        settings = JSON.stringify(settings);
      } catch { return json({error:'名称或处理参数格式错误。'},400); }
      if (!name.trim() || name.length > 80 || /[\u0000-\u001f]/.test(name) || settings.length > 1024) return json({error:'名称或处理参数过长。'},400);
      const bitmap = await readBitmap(request);
      if (!bitmap) return json({error:'点阵必须为400×300、15000字节。'},400);
      const sha256 = await bitmapHash(bitmap);
      const result = await db.prepare(`INSERT INTO album_proof (id,name,bitmap,sha256,settings,created_at)
        SELECT ?,?,?,?,?,? WHERE (SELECT COUNT(*) FROM album_proof) < 20 OR EXISTS (SELECT 1 FROM album_proof WHERE id=?)
        ON CONFLICT(id) DO UPDATE SET name=excluded.name, settings=excluded.settings`)
        .bind(sha256,name,bitmap,sha256,settings,Date.now(),sha256).run();
      if (!result.meta.changes) return json({error:'测试库已满（20张），完整相册将在后续步骤实现。'},409);
      const metrics = {rowsRead:result.meta.rows_read, rowsWritten:result.meta.rows_written};
      console.log(JSON.stringify({operation:'album-proof-write',bytes:SIZE,...metrics}));
      return json({id:sha256,sha256,bytes:SIZE,...metrics},201);
    }
    const id = /^\/([a-f0-9]{64})$/.exec(path)?.[1];
    if (id && ['GET','HEAD'].includes(request.method)) {
      const result = await db.prepare('SELECT bitmap,sha256,name FROM album_proof WHERE id=?').bind(id).all();
      const row = result.results[0];
      if (!row) return json({error:'测试图片不存在。'},404);
      const bitmap = Uint8Array.from(row.bitmap);
      if (bitmap.length !== SIZE) return json({error:'点阵数据不完整。'},503);
      console.log(JSON.stringify({operation:'album-proof-read',bytes:SIZE,rowsRead:result.meta.rows_read,rowsWritten:result.meta.rows_written}));
      return new Response(request.method === 'HEAD' ? null : bitmap,{headers:{...headers,
        'Content-Type':'application/octet-stream','Content-Length':String(SIZE),
        'X-Image-Sha256':row.sha256,'X-Photo-Name':encodeURIComponent(row.name),
        'X-D1-Rows-Read':String(result.meta.rows_read)}});
    }
    return json({error:'测试接口不存在。'},404);
  } catch (error) {
    console.error(JSON.stringify({operation:'album-proof',errorType:error.name}));
    return json({error:'测试存储暂时不可用，请稍后重试。'},503);
  }
}
