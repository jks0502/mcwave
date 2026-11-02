import { readFile, writeFile, mkdir } from 'node:fs/promises';
import {applyWaitingStatus,setControlStatus} from './tools/control-waiting-transform.mjs';
import {SITE_VERSION} from './site-version.mjs';

// Dashboard Quick Edit accepts one module. Bundle only page assets and the font
// and the existing connection test; never include project files or credentials.
const base = new URL('./', import.meta.url);
const files = {};
for (const [route, file, type] of [
  ['/', 'index.html', 'text/html'],
  ['/projects/', 'projects/index.html', 'text/html'],
  ['/about/', 'about/index.html', 'text/html'],
  ['/epaper/', 'epaper/index.html', 'text/html'],
  ['/epaper/album/', 'epaper/album/index.html', 'text/html'],
  ['/epaper/album/library/', 'epaper/album/library/index.html', 'text/html'],
  ['/epaper/album/upload/', 'epaper/album/upload/index.html', 'text/html'],
  ['/assets/album-consumer.css', 'assets/album-consumer.css', 'text/css'],
  ['/assets/album-consumer.js', 'assets/album-consumer.js', 'text/javascript'],
  ['/assets/album-drag.mjs', 'assets/album-drag.mjs', 'text/javascript'],
  ['/assets/album.css', 'assets/album.css', 'text/css'],
  ['/assets/control-album.css', 'assets/control-album.css', 'text/css'],
  ['/assets/control-waiting.css', 'assets/control-waiting.css', 'text/css'],
  ['/assets/album.js', 'assets/album.js', 'text/javascript'],
  ['/assets/album-player.mjs', 'assets/album-player.mjs', 'text/javascript'],
  ['/assets/album-bitmap.mjs', 'assets/album-bitmap.mjs', 'text/javascript'],
  ['/assets/styles.css', 'assets/styles.css', 'text/css'],
  ['/assets/main.js', 'assets/main.js', 'text/javascript']
]) {
  files[route] = { body:await readFile(new URL(`dist/${file}`,base),'utf8'), type:`${type}; charset=utf-8` };
}
files['/assets/main.js'].body = applyWaitingStatus(files['/assets/main.js'].body);
files['/assets/control-status.mjs'] = {body: 'export '+setControlStatus.toString(), type:'text/javascript; charset=utf-8'};
files['/epaper/'].body = files['/epaper/'].body.replace('</head>', '<link rel="stylesheet" href="/assets/control-waiting.css?v=1.6"></head>');
files['/epaper/'].body = files['/epaper/'].body.replace(/<p class="control-hint">[\s\S]*?<\/p>/, '').replace(/<form class="logout-form"[\s\S]*?<\/form>/, '');
files['/assets/fonts/source-serif-4-roman.woff2'] = {
  body:(await readFile(new URL('dist/assets/fonts/source-serif-4-roman.woff2',base))).toString('base64'),
  type:'font/woff2', encoding:'base64'
};
files['/assets/fonts/anthropic-serif-web.woff2'] = {
  body:(await readFile(new URL('dist/assets/fonts/anthropic-serif-web.woff2',base))).toString('base64'),
  type:'font/woff2', encoding:'base64'
};
files['/assets/jks-mark.png'] = {
  body:(await readFile(new URL('dist/assets/jks-mark.png',base))).toString('base64'),
  type:'image/png', encoding:'base64'
};
for (const name of ['forest','river','mountain']) files[`/assets/album-art-${name}.jpg`] = {body:(await readFile(new URL(`dist/assets/album-art-${name}.jpg`,base))).toString('base64'),type:'image/jpeg',encoding:'base64'};
const multiFiles={};
for(const [route,path,type] of [
 ['/assets/md-base.css','drafts/multi-device-v1/dist/assets/site-base.css','text/css'],
 ['/assets/md.css','drafts/multi-device-v1/dist/assets/app.css','text/css'],
 ['/assets/md.mjs','multi-device-ui/app.mjs','text/javascript'],
 ['/assets/md-screen.mjs','multi-device-ui/screen.mjs','text/javascript'],
 ['/assets/md-album.mjs','multi-device-ui/album.mjs','text/javascript'],
 ['/assets/mood.mjs','multi-device-ui/mood.mjs','text/javascript'],
 ['/assets/mood-art.mjs','multi-device-ui/mood-art.mjs','text/javascript'],
 ['/assets/mood.css','multi-device-ui/mood.css','text/css']
])multiFiles[route]={body:await readFile(new URL(path,base),'utf8'),type:type+'; charset=utf-8'};
multiFiles['/assets/md.css'].body+=await readFile(new URL('multi-device-ui/overrides.css',base),'utf8');
multiFiles['/assets/md.css'].body+=await readFile(new URL('multi-device-ui/album.css',base),'utf8');
const multiHtml=(await readFile(new URL('multi-device-ui/index.html',base),'utf8')).replaceAll('__SITE_VERSION__',SITE_VERSION);
const moodHtml=await readFile(new URL('multi-device-ui/mood.html',base),'utf8');
const testSource = (await readFile(new URL('../cloudflare-test/worker.mjs',base),'utf8')).replace('export default {','const connectionTest = {');
const loginHtml = await readFile(new URL('login.html', base), 'utf8');
const handler = await readFile(new URL('worker-handler.mjs', base), 'utf8');
const aiUsage = await readFile(new URL('aiusage-handler.mjs', base), 'utf8');
const control = await readFile(new URL('control-handler.mjs', base), 'utf8');
await mkdir(new URL('build/',base),{recursive:true});
await writeFile(new URL('build/worker.mjs',base),`import {moodWeb,moodDevice,moodVersions} from '../mood-api.mjs';\nconst MOOD_HTML = ${JSON.stringify(moodHtml)};\nimport {multiAlbumEnabled} from '../multi-album-api.mjs';\nimport {authorizeLegacyDevice,legacyBinding,albumCapableFirmware} from '../legacy-device-bridge.mjs';\nimport {multiDeviceEnabled,multiDeviceWebPath,multiDeviceWeb,multiDeviceSync,multiDeviceController,legacyOtaReply,legacyFirmware} from '../multi-device-api.mjs';\nimport {albumProof} from '../album-store.mjs';\nimport {albumApi,albumDevice,albumEnabled,pickAlbumPhoto} from '../album-api.mjs';\n${testSource}\nconst MULTI_DEVICE_FILES = ${JSON.stringify(multiFiles)};\nconst MULTI_DEVICE_HTML = ${JSON.stringify(multiHtml)};\nconst PUBLIC_FILES = ${JSON.stringify(files)};\nconst LOGIN_HTML = ${JSON.stringify(loginHtml)};\n${control}\n${aiUsage}\n${handler}`);
console.log('Built personal-site/build/worker.mjs');
