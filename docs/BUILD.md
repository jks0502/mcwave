# 复刻教程

## 0. 最小可用（只要一块会显示天气的板子）

1. 接好驱动板与屏幕，拨码 1=B、2=ON。
2. Arduino IDE 装 ESP32 开发板包 3.x（作者用 3.3.10）。
3. 复制 `firmware/Secrets.example.h` → `Secrets.h`，填热点；其余 `*.example.h` 同样复制并填空串（编译需要这些文件存在）：`CampusSecrets.h`、`NjitSecrets.h`、`MultiNetworkSecrets.h`、`RemoteSecrets.h`。
4. 准备配图数据：自备素材，用 `tools/pack_mcwave_assets.py` 生成 `PackedAssetsData.h`、`NailongSprite.h`（本仓库不含）。
5. 开发板选 `ESP32 Dev Module`、Flash 4MB、自定义分区表 `firmware/partitions.csv`，编译需要链接参数（见 `tools/build-mcwave.ps1`）：
   `-Wl,--wrap=vApplicationGetTimerTaskMemory,--wrap=dns_clear_cache,--wrap=esp_mbedtls_mem_calloc,--wrap=esp_mbedtls_mem_free`
   并加宏 `MCWAVE_MULTI_NETWORK`。
6. 首次 USB 烧录用 `tools/flash-mcwave.ps1 -Layout`（写 bootloader + 分区表 + `boot_app0` + 应用，并擦除照片缓存区；NVS 不动）。脚本中 `AA:BB:CC:DD:EE:FF` 是占位 MAC，`esptool` / `arduino-cli` 路径按本机修改。
   **以后 USB 烧录一定要同时写 `boot_app0`，不要只写 0x10000。改分区那次先整片备份。**

## 1. 网站后端（可选）

1. Cloudflare 账号 + Wrangler CLI；建 D1 数据库。
2. 按顺序执行 `site-backend/*schema.sql` 与 `migrations/`。
3. `worker-handler.mjs` 是请求入口，`build-worker.mjs` 依赖未随仓库提供的前端资源，仅供参考，不能直接构建；设备令牌用 `wrangler secret put` 写入，**不要提交到仓库**。
4. 固件里全局搜索 `jks0502.xyz`，换成你的域名（`RemoteSecrets.h`、`TlsTrust.h`、`SiteAddresses.cpp`、`CooperativeSecureClient.h`）。
5. 前端页面（登录、相册、心情、设备页）不在本仓库，后端接口见各 `*-api.mjs`。

## 2. OTA 发布

`tools/publish-firmware.ps1 -Board mcwave`（先加 `-DryRun`）。`tools/firmware-boards.json` 里的设备 ID 是占位，换成你自己的。

## 3. USB 网关

见 `gateway/README.md`。调试：`python gateway.py --port COM3 --console`；打包：`python -m PyInstaller --onefile --noconsole --name McwaveGateway --paths . gateway.py`。

## 4. 外壳

`enclosure/打印说明.txt`：三个 STL 各打一件，单位 mm，1:1，禁止自动缩放。作者实际用电工胶带粘合底座，未装前框。

## 检查点

- 串口 115200 能看到启动日志与版本号；
- 屏幕出现天气页；
- 短按 BOOT 切换到心情页（需要网站心情数据）；
- 网站能看到设备上报的版本，镜像页能看到当前画面。
