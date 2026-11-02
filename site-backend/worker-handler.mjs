// PUBLIC_FILES, LOGIN_HTML and connectionTest are injected by build-worker.mjs.
const SESSION_SECONDS = 12 * 60 * 60;
const encoder = new TextEncoder();
const privateHeaders = {
  'Cache-Control': 'no-store',
  'X-Content-Type-Options': 'nosniff',
  'Referrer-Policy': 'no-referrer',
  'X-Frame-Options': 'DENY',
  'Content-Security-Policy': "default-src 'self'; script-src 'self'; style-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'"
};

function loginPage(request, error = '', status = 200, extra = {}) {
  return new Response(request.method === 'HEAD' ? null : LOGIN_HTML.replace('{{ERROR}}', error), {
    status, headers: {...privateHeaders, 'Content-Type': 'text/html; charset=utf-8', ...extra}
  });
}

function redirect(location, extra = {}) {
  return new Response(null, {status: 303, headers: {...privateHeaders, Location: location, ...extra}});
}

async function sessionKey(env) {
  // Including the password also invalidates existing sessions when it changes.
  return crypto.subtle.importKey('raw', encoder.encode(`${env.SESSION_SECRET}:${env.EPAPER_PASSWORD}`),
    {name: 'HMAC', hash: 'SHA-256'}, false, ['sign', 'verify']);
}

function encodeSignature(bytes) {
  return btoa(String.fromCharCode(...new Uint8Array(bytes))).replaceAll('+', '-').replaceAll('/', '_').replace(/=+$/, '');
}

// 1.18.0: momosaysay's own password signs "m1" sessions. Same access as jks;
// the session only decides which mood half is hers.
async function moodKey(env) {
  return crypto.subtle.importKey('raw', encoder.encode(`${env.SESSION_SECRET}:momo:${env.MOOD_PASSWORD_MOMO}`),
    {name: 'HMAC', hash: 'SHA-256'}, false, ['sign', 'verify']);
}
const moodPasswordSet = env => /^\d{6}$/.test(env.MOOD_PASSWORD_MOMO || '') && env.MOOD_PASSWORD_MOMO !== env.EPAPER_PASSWORD;

// Mood side of a valid session: 0 = jks, 1 = momosaysay; null otherwise.
async function sessionSide(request, env, key) {
  const token = /(?:^|;\s*)epaper_session=([^;]+)/.exec(request.headers.get('Cookie') || '')?.[1];
  if (!token || token.length > 200) return null;
  const parts = token.split('.');
  if (parts.length !== 4 || !['v1', 'm1'].includes(parts[0]) || !/^\d{10}$/.test(parts[1]) ||
      !/^[a-f0-9-]{36}$/.test(parts[2]) || !/^[A-Za-z0-9_-]{43}$/.test(parts[3])) return null;
  if (parts[0] === 'm1' && !moodPasswordSet(env)) return null;
  const now = Math.floor(Date.now() / 1000);
  const expiry = Number(parts[1]);
  if (expiry <= now || expiry > now + SESSION_SECONDS) return null;
  const signature = Uint8Array.from(atob(parts[3].replaceAll('-', '+').replaceAll('_', '/') + '='), c => c.charCodeAt(0));
  const verified = await crypto.subtle.verify('HMAC', parts[0] === 'v1' ? key : await moodKey(env), signature, encoder.encode(parts.slice(0, 3).join('.')));
  return verified ? (parts[0] === 'v1' ? 0 : 1) : null;
}

function cookieAttributes(url) {
  return `Path=/; HttpOnly; SameSite=Strict${url.protocol === 'https:' ? '; Secure' : ''}`;
}

async function readPassword(request) {
  // Bound the streamed body as well as Content-Length; chunked input is bounded too.
  if (!request.headers.get('Content-Type')?.startsWith('application/x-www-form-urlencoded') ||
      Number(request.headers.get('Content-Length') || 0) > 256) return null;
  const reader = request.body?.getReader();
  if (!reader) return null;
  let size = 0;
  const chunks = [];
  try {
    for (;;) {
      const {done, value} = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > 256) { await reader.cancel(); return null; }
      chunks.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
  const form = new URLSearchParams(new TextDecoder().decode(bytes));
  const password = form.get('password');
  return form.getAll('password').length === 1 && /^\d{6}$/.test(password || '') ? password : null;
}

async function handlePrivate(request, env, url) {
  const local = ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname);
  if (url.protocol !== 'https:' && !local) {
    url.protocol = 'https:';
    return new Response(null, {status: 308, headers: {...privateHeaders, Location: url.toString()}});
  }
  if (!['GET', 'HEAD', 'POST'].includes(request.method)) {
    return new Response('Method not allowed', {status: 405, headers: {...privateHeaders, Allow: 'GET, HEAD, POST'}});
  }
  const multiPath = multiDeviceWebPath(url.pathname);
  if (multiPath && !multiDeviceEnabled(env)) return controlJson({error:'Not found'},404);
  const isLogin = url.pathname === '/epaper/login';
  const isLogout = url.pathname === '/epaper/logout';
  if (request.method === 'POST') {
    const localAlbum = local && env.LOCAL_PREVIEW === 'true' && url.pathname === '/api/epaper/album-proof';
    const manageAlbum = albumEnabled(env,url) && (url.pathname === '/api/epaper/album' || url.pathname.startsWith('/api/epaper/album/'));
    if (!isLogin && !isLogout && url.pathname !== '/api/epaper/command' && url.pathname !== '/api/epaper/mood' && !localAlbum && !manageAlbum && !multiPath) return new Response('Method not allowed', {status: 405, headers: privateHeaders});
    const source = request.headers.get('Origin');
    // Codex's embedded preview submits forms with an opaque Origin. This is
    // enabled only by the local config and only on a loopback URL.
    const localPreview = local && env.LOCAL_PREVIEW === 'true' && source === 'null';
    // Opaque embedded-browser origins still carry browser-enforced fetch metadata.
    const sameOriginOpaque = source === 'null' && request.headers.get('Sec-Fetch-Site') === 'same-origin';
    if (source !== url.origin && !localPreview && !sameOriginOpaque) return new Response('Forbidden', {status: 403, headers: privateHeaders});
    if (isLogout) return redirect('/epaper/login', {'Set-Cookie': `epaper_session=; Max-Age=0; ${cookieAttributes(url)}`});
  }
  if (!env.EPAPER_PASSWORD || !env.SESSION_SECRET || env.SESSION_SECRET.length < 32 || !env.LOGIN_LIMITER) {
    return loginPage(request, '访问暂未配置好，请稍后再试。', 503);
  }
  const key = await sessionKey(env);
  const side = await sessionSide(request, env, key);
  const signedIn = side !== null;
  if (isLogin && request.method === 'POST') {
    const {success} = await env.LOGIN_LIMITER.limit({key: `epaper:${request.headers.get('CF-Connecting-IP') || 'local'}`});
    if (!success) return loginPage(request, '尝试次数较多，请一分钟后再试。', 429, {'Retry-After': '60'});
    const password = await readPassword(request);
    // Web Crypto verifies the password MAC without a direct secret comparison.
    const expected = await crypto.subtle.sign('HMAC', key, encoder.encode(env.EPAPER_PASSWORD));
    let prefix = null, signer = key;
    if (password !== null && await crypto.subtle.verify('HMAC', key, expected, encoder.encode(password))) prefix = 'v1';
    else if (password !== null && moodPasswordSet(env)) {
      signer = await moodKey(env);
      const mood = await crypto.subtle.sign('HMAC', signer, encoder.encode(env.MOOD_PASSWORD_MOMO));
      if (await crypto.subtle.verify('HMAC', signer, mood, encoder.encode(password))) prefix = 'm1';
    }
    if (!prefix) return loginPage(request, '密码不正确，请重新输入。', 401);
    const payload = `${prefix}.${Math.floor(Date.now() / 1000) + SESSION_SECONDS}.${crypto.randomUUID()}`;
    const signature = encodeSignature(await crypto.subtle.sign('HMAC', signer, encoder.encode(payload)));
    return redirect('/epaper/', {'Set-Cookie': `epaper_session=${payload}.${signature}; Max-Age=${SESSION_SECONDS}; ${cookieAttributes(url)}`});
  }
  if (isLogin) return signedIn ? redirect('/epaper/') : loginPage(request);
  if (isLogout) return redirect(signedIn ? '/epaper/' : '/epaper/login');
  if (!signedIn) return multiPath || url.pathname === '/api/epaper/mood' ? controlJson({error:'Please sign in'},401) : redirect('/epaper/login');
  if (url.pathname === '/api/epaper/mood') return moodWeb(request, env, side);
  if (url.pathname === '/epaper/mood') return redirect('/epaper/mood/');
  if (url.pathname === '/epaper/mood/') return new Response(request.method === 'HEAD' ? null : MOOD_HTML, {headers: {...privateHeaders, 'Content-Type': 'text/html; charset=utf-8'}});
  if (multiPath) return multiDeviceWeb(request,env,url,{control:handleControl});
  if (url.pathname === '/api/epaper/album' || url.pathname.startsWith('/api/epaper/album/')) {
    if (!albumEnabled(env,url)) return new Response('Not found',{status:404,headers:privateHeaders});
    return albumApi(request,env,url.pathname.slice('/api/epaper/album'.length)||'/');
  }
  if (url.pathname === '/api/epaper/album-proof' || url.pathname.startsWith('/api/epaper/album-proof/')) {
    if (!local || env.LOCAL_PREVIEW !== 'true') return new Response('Not found',{status:404,headers:privateHeaders});
    const path = url.pathname.slice('/api/epaper/album-proof'.length) || '/';
    return albumProof(request,env.ALBUM_PROOF_DB,path);
  }
  if (url.pathname.startsWith('/api/epaper/')) return handleControl(request, env, url);
  if (url.pathname === '/epaper' || url.pathname === '/epaper/index.html') return redirect('/epaper/');
  if(multiAlbumEnabled(env)&&/^\/epaper\/devices\/[a-f0-9-]{36}\/album\/(?:library\/|upload\/)?$/.test(url.pathname))return new Response(request.method==='HEAD'?null:MULTI_DEVICE_HTML.replace('/assets/md.mjs','/assets/md-album.mjs'),{headers:{...privateHeaders,'Content-Type':'text/html; charset=utf-8'}});
  if(multiDeviceEnabled(env)&&(url.pathname==='/epaper/'||/^\/epaper\/devices\/[a-f0-9-]{36}\/$/.test(url.pathname)))return new Response(request.method==='HEAD'?null:MULTI_DEVICE_HTML,{headers:{...privateHeaders,'Content-Type':'text/html; charset=utf-8'}});
  const albumPages=['/epaper/album/','/epaper/album/library/','/epaper/album/upload/'];
  if(albumEnabled(env,url)){
    const alias={'/epaper/album':'/epaper/album/','/epaper/album/index.html':'/epaper/album/','/epaper/album/library':'/epaper/album/library/','/epaper/album/library.html':'/epaper/album/library/','/epaper/album/upload':'/epaper/album/upload/','/epaper/album/upload.html':'/epaper/album/upload/'}[url.pathname];
    if(alias)return redirect(alias+url.search);
  }
  const albumPage = albumEnabled(env,url) && albumPages.includes(url.pathname);
  if (url.pathname !== '/epaper/' && !albumPage) return new Response('Not found', {status: 404, headers: privateHeaders});
  let page = PUBLIC_FILES[albumPage ? url.pathname : '/epaper/'].body;
  if (!albumPage && albumEnabled(env,url)) page = page.replace('<button type="button" data-display="sports" aria-pressed="false">赛事速览</button>', '<button type="button" data-display="sports" aria-pressed="false">赛事速览</button><a class="album-entry" href="/epaper/album/">云相册</a>');
  return new Response(request.method === 'HEAD' ? null : page, {
    headers: {...privateHeaders, 'Content-Type': 'text/html; charset=utf-8'}
  });
}

const STATIC_ENTRIES = new WeakMap();
async function staticEntry(file) {
  let entry = STATIC_ENTRIES.get(file);
  if (!entry) {
    const body = file.encoding === 'base64' ? Uint8Array.from(atob(file.body), character => character.charCodeAt(0)) : file.body;
    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', typeof body === 'string' ? new TextEncoder().encode(body) : body));
    entry = {body, etag: '"' + Array.from(digest.slice(0, 8), byte => byte.toString(16).padStart(2, '0')).join('') + '"'};
    STATIC_ENTRIES.set(file, entry);
  }
  return entry;
}

export default {
  async fetch(request, env = {}) {
    const url = new URL(request.url);
    if (url.pathname === "/api/aiusage" || url.pathname.startsWith("/api/aiusage/")) return handleAiUsage(request,env);
    if (url.pathname === '/api/device/album' || url.pathname.startsWith('/api/device/album/')) return albumDevice(request,env,url);
    if (url.pathname === '/api/device/sync') return handleDevice(request, env, url);
    const moodPath = /^\/api\/device\/mood\/([01])$/.exec(url.pathname);
    if (moodPath) return handleDeviceMood(request, env, url, Number(moodPath[1]));
    const legacyFirmwarePath = /^\/api\/device\/firmware\/([a-f0-9]{64})$/.exec(url.pathname);
    if (legacyFirmwarePath) return handleDeviceFirmware(request, env, url, legacyFirmwarePath[1]);
    if (url.pathname.startsWith('/api/v2/controller/')) return multiDeviceController(request,env,url,{control:handleControl});
    if (url.pathname.startsWith('/api/v2/device/')) return multiDeviceSync(request,env,url);
    if (url.pathname === '/epaper' || url.pathname.startsWith('/epaper/') ||
        url.pathname === '/api/epaper' || url.pathname.startsWith('/api/epaper/')) {
      return handlePrivate(request, env, url);
    }
    const headers = {'Cache-Control': 'no-cache', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer'};
    if (!['GET', 'HEAD'].includes(request.method)) return new Response('Method not allowed', {status: 405, headers: {...headers, Allow: 'GET, HEAD'}});
    if (['/projects', '/about'].includes(url.pathname)) return new Response(null, {status:308, headers:{...headers, Location:`${url.pathname}/${url.search}`}});
    if (url.pathname === '/game') return new Response(null, {status:308, headers:{...headers, Location:'/game/'+url.search}});
    if (url.pathname.startsWith('/game/') && env.GAME_ASSETS) return env.GAME_ASSETS.fetch(request);
    // 1.18.3: the mood page's web font is a static asset (too large for the Worker bundle).
    if (url.pathname.startsWith('/fonts/') && env.GAME_ASSETS) return env.GAME_ASSETS.fetch(request);
    const file = (multiDeviceEnabled(env)&&MULTI_DEVICE_FILES[url.pathname])||PUBLIC_FILES[url.pathname];
    if (file) {
      // 1.24.2: files carry an ETag (so a revalidation is a tiny 304 instead of the whole body); images and
      // fonts may also be reused for a day. Decoded bytes are kept per isolate instead of re-decoded per request.
      const entry = await staticEntry(file);
      const cached = /^(image|font)\//.test(file.type);
      const fileHeaders = {...headers, 'Content-Type': file.type, 'ETag': entry.etag, 'Cache-Control': cached ? 'public, max-age=86400' : 'no-cache'};
      const match = request.headers.get('If-None-Match');
      if (match && match.split(',').some(tag => tag.trim().replace(/^W\//, '') === entry.etag)) return new Response(null, {status: 304, headers: fileHeaders});
      return new Response(request.method === 'HEAD' ? null : entry.body, {headers: fileHeaders});
    }
    if (['/connection-test', '/connection-test/'].includes(url.pathname)) { url.pathname = '/'; return connectionTest.fetch(new Request(url, request)); }
    return connectionTest.fetch(request);
  }
};
