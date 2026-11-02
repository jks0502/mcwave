# mcwave USB 网关（mcwave 7.0.0）

mcwave 插到 Windows 电脑的 USB 口时，由这台电脑给它提供**时间**和**网络**，天气、云相册、心情、网站远程控制、屏幕镜像、无线升级都照常工作。没插电脑时，mcwave 仍按 NJIT → moyimi 自己连 Wi-Fi。

## 她第一次使用

1. 把 `McwaveGateway.exe`、`Install.cmd`、`install.ps1`、`uninstall.ps1`、`Uninstall.cmd` 放在同一个文件夹，双击 **Install.cmd**（不需要管理员权限）。
2. 用**能传数据的** USB 线把 mcwave 插到电脑。Windows 11 会自动识别串口，不用装驱动。
3. 几秒后屏幕开始更新。

## 以后

只要插上 mcwave 的 USB 线、电脑开着并能上网。网关随开机自动运行，没有窗口。拔掉就停，再插上自动恢复。电脑休眠或关机时，屏幕保留最后一帧。

## 它做什么、不做什么

- 固件自己做 HTTPS 加密和证书校验；网关只负责把加密后的字节转发到目标服务器，**不保存、也看不到任何账号、密码或令牌**。
- 电脑端不需要配置：不填 Wi-Fi、不登录。

## 排查

- 运行记录：`%LOCALAPPDATA%\McwaveGateway\gateway.log`；当前状态：同目录 `status.json`。
- 看到 `attached` 表示已连上设备；`probing` 一直重复说明没找到设备（换数据线、换 USB 口）。
- 同一时间只能有一个程序占用串口：不要同时开串口助手。
- 卸载：双击 **Uninstall.cmd**。

## 开发

- 协议：`protocol.py` 与固件 `WeatherClock/UsbFrame.h` 一致，帧格式见文件头；主机→设备的数据带字节偏移，设备按序确认，网关未收到确认就重发（ESP32 的串口没有硬件流控，写 Flash 时会丢字节）。
- 测试：`python tests/test_mcwave_gateway.py`（含丢帧重传）；固件侧 `tests/test_usb_frame.cpp`。
- 调试运行：`python gateway.py --port COM3 --console`。
- 打包：`python -m PyInstaller --onefile --noconsole --name McwaveGateway --paths . gateway.py`。
