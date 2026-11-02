"""Create the local, configured source folder and record build/upload state."""
import argparse
from pathlib import Path
import hashlib
import json
import re
import shutil

project = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--uploaded", action="store_true", help="record that this build was uploaded to the device")
args = parser.parse_args()
version = re.search(r'kFirmwareVersion\[\] = "([^"]+)"',
                    (project / "WeatherClock" / "ClockConfig.h").read_text(encoding="utf-8")).group(1)
release = project / "releases" / version
release.mkdir(parents=True, exist_ok=True)
shutil.copytree(project / "WeatherClock", release / "WeatherClock", dirs_exist_ok=True,
                ignore=shutil.ignore_patterns("build","OtaSecrets.h"))
(release / "tools").mkdir(exist_ok=True)
shutil.copyfile(project / "tools/build.ps1",release / "tools/build.ps1")
instructions = f"""富哥产品2 {version} 手动上传与验证

1. 打开完整文件夹中的 WeatherClock/WeatherClock.ino。
   已包含当前校园网账号配置和原热点配置；不要只复制 ino 文件。
2. Arduino IDE 设置：ESP32 Dev Module；Flash Size 4MB；
   Partition Scheme = Huge APP (3MB No OTA/1MB SPIFFS)；
   Upload Speed = 115200；Port = COM3（或插板后出现的端口）。
3. 先在完整文件夹根目录执行 powershell -ExecutionPolicy Bypass -File tools/build.ps1。
   构建脚本包含启动内存和DNS线程安全修复所需链接参数，不要用Arduino IDE默认勾号编译替代。
   编译后使用Arduino CLI通过USB上传firmware目录中的完整分区及应用。
   到学校的 SEU-WLAN（2.4GHz）覆盖范围内上电。
   默认自动连接校园网并进行网页认证，不需要手机热点。
   CampusSecrets.h 的 kCampusEnabled 改为 false 可切回原 iPhone 热点。
4. 等待认证、校时与数据查询；首次查询可能需要几分钟。
   正常天气应出现时间、日期、温度和天气；短按 BOOT 切赛事，长按约1秒翻页。
5. 当前默认适配 HINK-E042A03-A1（400×300黑白屏）：每次全刷后休眠。
   自动画面更新至少间隔60秒，切页/翻页仍立即全刷；全刷短暂闪烁正常。
   Rev 3驱动板沿用1号B（3Ω）、2号ON；调整硬件前断电。
   换回原微雪4.2inch V2屏时，把 ClockConfig.h 的
   CLOCK_HINK_FULL_REFRESH_ONLY 改为0，再重新上传，恢复原局刷规则。
6. 串口监视器选择 115200。成功日志包括：
   [screen] panel=HINK full-only auto interval=60000 ms
   [screen] HINK full refresh; asleep
   [campus] state=已认证
   [weather] HTTP=200
   [weather] NMC updated ...
   [rain] NMC slots=...; China Weather probability=updated
   [sports] domestic snapshot events=... errors=0
   [sports] parse=OK ...
   HTTP=200 仅代表收到响应，updated / parse=OK 才代表解析成功。
7. 若仍失败，复制从启动起的 [screen]、[wifi]、[campus]、TLS、HTTP、heap 行。
   不要发送账号密码、配置文件或带登录参数的地址。

天气和降雨时点使用中央气象台，概率使用中国天气通/华风爱科江宁公开看板。
两者均不需要Token；详细来源及时间精度见同文件夹的降雨预报说明.md。
赛事仍使用腾讯体育、5E 国内源：任意入选比赛（含补充）进行中每分钟查询，否则每半小时查询。
首次/恢复联网、北京时间跨日立即查询；预计开赛时间额外检查一次，查询失败保留已有数据。
切换页面不改变查询频率，屏幕和网站同步继续原周期。
每次天气成功及完整赛事成功都保存到板子存储；断网、断电、重启后先显示缓存。
天气缓存需要新版先成功查询一次。赛事未校时/跨日时按缓存日期展示，可继续翻页。
天气请求失败时显示上次成功的数据，状态提示“未连接”，数据时间标注“缓存”。
沿用赛事低内存解析；启动缓存显示“缓存待更新”。
沿用V3.4看门狗修复：每次HTTPS读取等待一个RTOS tick。
认证每分钟检查，失败自动重试；明确登录被拒绝后等待五分钟。
刚上电的TLS时间种子不会作为准确时钟显示；支持NTP或验证HTTPS响应校时。
上传后保持运行至少十分钟，观察每次全刷后画面是否稳定、文字对比度是否正常。
若持续闪烁或灰暗卡住，保留 Screen BUSY timeout 等屏幕错误日志。
串口如再出现 task_wdt / Rebooting / rst:0xc，请保存前后日志。
天气和降雨每10分钟查询。中央分时预报间隔3小时，时间表示预报时点。
天气右侧显示下次降雨的日期/时点及相同小时概率，缺失显示“概率暂无”。
断网保留温湿度和上次降雨预报，重启恢复缓存；概率单独失败可显示带“旧”的缓存概率。
概率公开看板实测约0.7MB/次，十分钟持续查询约100MB/天。
F1 数据源缺少车手排名/结果时显示“赛况待更新”。
电脑编译和模拟检查已通过。{"本次版本已上传并通过串口写入校验；屏幕外观仍需目视确认。" if args.uploaded else "尚未烧录到实物；HINK刷屏及校园网仍需现场验证。"}
"""
(release / "上传说明.txt").write_text(instructions, encoding="utf-8-sig")
shutil.copyfile(project / "CAMPUS_NETWORK.md", release / "校园网说明.md")
shutil.copyfile(project / "RAIN_SOURCES.md", release / "降雨预报说明.md")
if version.startswith(("V4.","V5.")):
    shutil.copyfile(project / "REMOTE_CONTROL.md", release / "网站远程控制说明.md")
if version.startswith("V5."):
    shutil.copyfile(project / "CLOUD_ALBUM_V5.md",release / "云相册说明.md")
if (project / "SPORTS_SELECTION.md").exists():
    shutil.copyfile(project / "SPORTS_SELECTION.md", release / "赛事筛选说明.md")
artifacts = []
for file in sorted((project / "firmware").glob("*.bin")):
    if "_flashed" in file.name:
        continue
    artifacts.append({"name": file.name, "bytes": file.stat().st_size,
                      "sha256": hashlib.sha256(file.read_bytes()).hexdigest()})
record = {"version": version, "target": "esp32:esp32:esp32:PartitionScheme=huge_app",
          "compiled": True, "uploaded": args.uploaded, "manual_validation_pending": True,
          "artifacts": artifacts}
(project / "firmware" / "verification.json").write_text(
    json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Source folder: {release / 'WeatherClock'}")
