function controlJson(body, status = 200) {
  return new Response(JSON.stringify(body), {status, headers: {...privateHeaders, 'Content-Type': 'application/json; charset=utf-8'}});
}

async function boundedJson(request) {
  if (!request.headers.get('Content-Type')?.startsWith('application/json') ||
      Number(request.headers.get('Content-Length') || 0) > 512) return null;
  const reader = request.body?.getReader();
  if (!reader) return null;
  let text = '', size = 0;
  const decoder = new TextDecoder();
  try {
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > 512) { await reader.cancel(); return null; }
      text += decoder.decode(value, {stream: true});
    }
    return JSON.parse(text + decoder.decode());
  } catch { return null; }
  finally { reader.releaseLock(); }
}

function validDisplay(body) {
  return body && ['weather', 'sports', 'album', 'mood'].includes(body.mode) &&
    Number.isInteger(body.page) && body.page >= 0 && body.page < 64 &&
    (body.mode === 'sports' || body.page === 0);
}

function publicControl(row) {
  return {...row, capabilities:{album:albumCapableFirmware(row.firmware)}, online: row.last_seen > Date.now() - 90000,
    pending: row.applied_revision < row.revision};
}

async function handleControl(request, env, url, {cooldownMs=0}={}) {
  if (!env.CONTROL_DB) return controlJson({error: '控制服务尚未配置'}, 503);
  try {
    if(request.method==='POST'){const bridge=await legacyBinding(env);if(bridge&&!bridge.enabled)return controlJson({error:'设备已停用'},409);}
    if (url.pathname === '/api/epaper/state' && request.method === 'GET') {
      const row = await env.CONTROL_DB.prepare('SELECT * FROM epaper_control WHERE id = 1').first();
      return row ? controlJson(publicControl(row)) : controlJson({error: '控制服务尚未初始化'}, 503);
    }
    if (url.pathname === '/api/epaper/command' && request.method === 'POST') {
      const body = await boundedJson(request);
      const now=Date.now(), oldest=cooldownMs?now-cooldownMs:Number.MAX_SAFE_INTEGER;
      if (!validDisplay(body) || !Number.isSafeInteger(body.baseRevision) || body.baseRevision < 0) {
        return controlJson({error: '无效的页面指令'}, 400);
      }
      if(body.mode==='album') {
        if(!albumEnabled(env,url))return controlJson({error:'相册尚未启用'},400);
        const previous=await env.CONTROL_DB.prepare('SELECT * FROM epaper_control WHERE id=1').first();
        if(!albumCapableFirmware(previous?.firmware))return controlJson({error:'请先接入支持相册的 V5.0 固件'},409);
        const first=previous.mode!=='album'&&!body.photoId?await env.ALBUM_DB?.prepare('SELECT current_id FROM album_state WHERE id=1').first():null;
        const photo=body.photoId||first?.current_id||(previous.applied_revision<previous.revision?previous.photo_id:previous.reported_photo)||previous.photo_id||'';
        if(photo&&!/^[a-f0-9]{64}$/.test(photo) || ![undefined,-1,1].includes(body.direction) ||
          body.paused!==undefined&&typeof body.paused!=='boolean' || body.random!==undefined&&typeof body.random!=='boolean' ||
          body.interval!==undefined&&![60,300,600].includes(body.interval))return controlJson({error:'无效播放设置'},400);
        let chosen;
        if(body.direction || !photo)chosen=await pickAlbumPhoto(env,photo,body.direction||1,body.random??!!previous.random_order);
        else chosen=await env.ALBUM_DB.prepare('SELECT id FROM album_photos WHERE id=? AND enabled=1 AND deleted_at IS NULL AND owner IS NULL').bind(photo).first();
        // A remembered photo that was deleted/disabled since is not the user's choice: start from the first active photo.
        if(!chosen&&!body.photoId)chosen=await pickAlbumPhoto(env,'',1,body.random??!!previous.random_order);
        if(!chosen)return controlJson({error:'请先把照片加入播放清单'},400);
        const row=await env.CONTROL_DB.prepare(`UPDATE epaper_control SET revision=revision+1,mode='album',page=0,photo_id=?,paused=?,random_order=?,interval_seconds=?,requested_at=? WHERE id=1 AND revision=? AND requested_at<=? RETURNING *`)
          .bind(chosen.id,Number(body.paused??!!previous.paused),Number(body.random??!!previous.random_order),body.interval??previous.interval_seconds,now,body.baseRevision,oldest).first();
        return row?controlJson(publicControl(row)):controlJson({error:'状态已变化，请重新选择'},409);
      }
      const row = await env.CONTROL_DB.prepare(
        'UPDATE epaper_control SET revision = revision + 1, mode = ?, page = ?, requested_at = ? WHERE id = 1 AND revision = ? AND requested_at <= ? RETURNING *'
      ).bind(body.mode, body.page, now, body.baseRevision, oldest).first();
      return row ? controlJson(publicControl(row)) : controlJson({error: '页面状态已变化，请重新选择'}, 409);
    }
    return controlJson({error: 'Not found'}, 404);
  } catch {
    console.error(JSON.stringify({event: 'control_database_failed'}));
    return controlJson({error: '控制服务暂不可用，请稍后重试'}, 503);
  }
}

async function handleDevice(request, env, url) {
  if (url.protocol !== 'https:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) return controlJson({error: 'HTTPS required'}, 400);
  if (request.method !== 'POST') return controlJson({error: 'Method not allowed'}, 405);
  if (!env.CONTROL_DB || (!multiDeviceEnabled(env)&&(!env.DEVICE_TOKEN || env.DEVICE_TOKEN.length < 32))) return controlJson({error: 'Not configured'}, 503);
  try {if (!await authorizeLegacyDevice(request,env)) return controlJson({error:'Unauthorized'},401);}
  catch {return controlJson({error:'Service unavailable'},503);}
  const body = await boundedJson(request);
  if (!validDisplay(body) || !Number.isSafeInteger(body.appliedRevision) || body.appliedRevision < 0 ||
      !Number.isInteger(body.pageCount) || body.pageCount < 1 || body.pageCount > 64 || body.page >= body.pageCount ||
      !/^V[0-9A-Za-z.-]{1,24}$/.test(body.firmware || '')) return controlJson({error: 'Invalid report'}, 400);
  const v5=albumCapableFirmware(body.firmware);
  if(body.mode==='album'&&!v5 || v5&&(!/^(?:[a-f0-9]{64})?$/.test(body.photoId??'') || !Number.isInteger(body.cacheCount)||body.cacheCount<0||body.cacheCount>20))return controlJson({error:'Invalid album report'},400);
  try {
    let row;
    if(v5)row=await env.CONTROL_DB.prepare(`UPDATE epaper_control SET applied_revision=?,reported_mode=?,reported_page=?,page_count=?,firmware=?,last_seen=?,reported_photo=?,cache_count=? WHERE id=1 AND revision>=? AND applied_revision<=? RETURNING revision,mode,page,photo_id,paused,random_order,interval_seconds`)
      .bind(body.appliedRevision,body.mode,body.page,body.pageCount,body.firmware,Date.now(),body.photoId||'',body.cacheCount,body.appliedRevision,body.appliedRevision).first();
    else row = await env.CONTROL_DB.prepare(
      'UPDATE epaper_control SET applied_revision = ?, reported_mode = ?, reported_page = ?, page_count = ?, firmware = ?, last_seen = ? WHERE id = 1 AND revision >= ? AND applied_revision <= ? RETURNING revision, mode, page'
    ).bind(body.appliedRevision, body.mode, body.page, body.pageCount, body.firmware, Date.now(), body.appliedRevision,body.appliedRevision).first();
    // A reboot may restore an older cached revision. Ignore that stale report,
    // but return the latest command using the old 200 wire format so V5 firmware
    // can recover without a protocol upgrade or losing the last good picture.
    if(!row) {
      const current=await env.CONTROL_DB.prepare(`SELECT ${v5?'revision,mode,page,photo_id,paused,random_order,interval_seconds':'revision,mode,page'} FROM epaper_control WHERE id=1 AND revision>=?`).bind(body.appliedRevision).first();
      if(current)row=current;
    }
    if(row&&v5) {
      row={revision:row.revision,mode:row.mode,page:row.page,photoId:row.photo_id,paused:!!row.paused,random:!!row.random_order,interval:row.interval_seconds};
      if(row.mode==='album') {
        const exists=await env.ALBUM_DB?.prepare('SELECT id FROM album_photos WHERE id=? AND deleted_at IS NULL AND enabled=1 AND owner IS NULL').bind(row.photoId).first();
        const reported=body.mode==='album'&&body.photoId?await env.ALBUM_DB?.prepare('SELECT id FROM album_photos WHERE id=? AND deleted_at IS NULL AND enabled=1 AND owner IS NULL').bind(body.photoId).first():null;
        if(!exists || body.mode==='album'&&!reported&&row.revision===body.appliedRevision) {
          const next=exists||await pickAlbumPhoto(env);
          if(!next)return controlJson({error:'播放清单为空；保留当前画面'},404);
          const repaired=await env.CONTROL_DB.prepare('UPDATE epaper_control SET revision=revision+1,photo_id=?,requested_at=? WHERE id=1 AND revision=? RETURNING revision').bind(next.id,Date.now(),row.revision).first();
          if(!repaired)return controlJson({error:'Revision conflict'},409);
          row.photoId=next.id;row.revision=repaired.revision;
        }
      }
    }
    if(!row)return controlJson({error: 'Revision conflict'}, 409);
    let offer={};
    try {const bridge=await legacyBinding(env);if(bridge)offer=await legacyOtaReply(env,bridge,body.firmware,body.ota);}
    catch {console.error(JSON.stringify({event:'legacy_ota_failed'}));}
    // mcwave 6.0.0 downloads a mood half when its version here changes.
    let mood={};
    try {mood={mood:await moodVersions(env)};}
    catch {console.error(JSON.stringify({event:'mood_versions_failed'}));}
    return controlJson({...row,...offer,...mood});
  } catch {
    console.error(JSON.stringify({event: 'device_database_failed'}));
    return controlJson({error: 'Service unavailable'}, 503);
  }
}

// mcwave 6.0.0 mood halves (see mood-api.mjs).
async function handleDeviceMood(request, env, url, side) {
  if (url.protocol !== 'https:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) return controlJson({error: 'HTTPS required'}, 400);
  if (request.method !== 'GET') return controlJson({error: 'Method not allowed'}, 405);
  if (!env.CONTROL_DB) return controlJson({error: 'Not configured'}, 503);
  try {
    if (!await authorizeLegacyDevice(request,env)) return controlJson({error:'Unauthorized'},401);
    return await moodDevice(env, side);
  } catch {return controlJson({error:'Service unavailable'},503);}
}

// mcwave OTA image ranges, same rules as /api/v2/device/firmware.
async function handleDeviceFirmware(request, env, url, sha) {
  if (url.protocol !== 'https:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) return controlJson({error: 'HTTPS required'}, 400);
  if (!env.CONTROL_DB || !multiDeviceEnabled(env)) return controlJson({error: 'Not configured'}, 503);
  try {
    if (!await authorizeLegacyDevice(request,env)) return controlJson({error:'Unauthorized'},401);
    const bridge=await legacyBinding(env);
    return bridge ? legacyFirmware(request,env,url,bridge,sha) : controlJson({error:'Not found'},404);
  } catch {return controlJson({error:'Service unavailable'},503);}
}
