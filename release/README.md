# ESP32-S3 E-Paper Firmware Flash Guide

本文档给不熟悉 ESP-IDF 的用户使用。只需要一个整包固件文件和 Espressif 下载工具即可完成烧录。

## 项目入口

```text
Gitee 固件仓库:       https://gitee.com/gxp666111/miaomiao
嘉立创开源硬件工程:  https://oshwhub.com/team_voosogmo/project_fxbcjhaa
```

## 固件文件

推荐下载入口：

```text
https://gitee.com/gxp666111/miaomiao/repository/archive/firmware-download.zip
```

为了避免 Gitee raw 单文件下载把文件名保存成带单引号的形式，建议从主页的“下载固件烧录包”入口下载，或打开这个专用分支打包下载。Gitee 下载到本地的文件通常叫：

```text
firmware-download.zip
```

解压后使用整包固件：

```text
epaper_uploader_full_16MB.bin
```

ZIP 内只包含上述整包固件文件；校验信息以本文件为准。

校验信息：

```text
Size:   12189696 bytes
SHA256: A551133833866F5BCA61E087362268124BF34AA7D70EEC06D1725CFF86D1EFBA
```

ZIP 校验信息：

```text
Size:   1797089 bytes
SHA256: 0564E11F9846773711A37AF6D50EC2AD1CC718AD7407FD66676C58090B04AD1D
```

如果手动点 raw 下载后文件名带了单引号，例如：

```text
'epaper_uploader_full_16MB.bin'
epaper_uploader_full_16MB.bin'
```

请手动重命名为：

```text
epaper_uploader_full_16MB.bin
```

这不是文件损坏，只是 Gitee raw 下载响应头导致的文件名问题。


适用范围：

```text
Chip:       ESP32-S3
Flash:      16MB
SPI Mode:   DIO
SPI Speed:  80MHz
App offset: 0x20000
```

## 下载工具一行烧录

在 Espressif Flash Download Tool 的 `SPIDownload` 页面只填一行：

```text
File:    release\epaper_uploader_full_16MB.bin
Address: 0x0000
```

推荐参数：

```text
SPI SPEED: 80MHz
SPI MODE:  DIO
DoNotChgBin: 勾选
BAUD: 115200 或 460800
COM: 选择设备对应串口
```

如果工具里有 Flash Size 选项，请选择：

```text
16MB
```

然后点击：

```text
START
```

等待下载完成后，设备会自动重启。

## 是否需要点 ERASE

新板子、配置混乱、SPIFFS 挂载异常时：

```text
可以先点 ERASE，再点 START
```

已经配置过 WiFi、天气、日历，不想丢配置时：

```text
不要点 ERASE，直接点 START
```

说明：

```text
整包固件会写入 bootloader、partition table、otadata 和 app。
不点 ERASE 时，NVS 配置区和 SPIFFS 数据区不会被主动清空。
```

## 整包内包含的分区

这个整包固件已经合并以下文件：

```text
0x0000   bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x15000  ota_data_initial.bin
0x20000  epaper_uploader.bin
0x920000 fontfs.bin
```

因此普通用户不需要再分别选择 4 个文件。

## 常见问题

### 下载失败或一直等待

请检查：

```text
1. COM 口是否选对
2. USB 数据线是否支持数据传输
3. 串口是否被 IDF monitor、Arduino、其他下载工具占用
4. 必要时按住 BOOT 键，再点 START，开始后松开 BOOT
```

### 烧录后还是旧配置

如果希望清空旧 WiFi、天气、面板等配置：

```text
先点 ERASE，再点 START
```

### 烧录后无法访问设备

如果没有保存 WiFi 配置，设备会开启 AP：

```text
SSID: ESP32_EPD_xxxxxx
Web:  http://192.168.4.1/
```

连接 AP 后进入网页配置 WiFi、天气、Codex 额度和屏幕参数。

## 开发者重新生成整包

在 ESP-IDF 环境中先编译：

```powershell
idf.py build
```

然后合并固件：

```powershell
python -m esptool --chip esp32s3 merge_bin `
  -o release\epaper_uploader_full_16MB.bin `
  --flash_mode dio --flash_freq 80m --flash_size 16MB `
  0x0 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x15000 build\ota_data_initial.bin `
  0x20000 build\epaper_uploader.bin `
  0x920000 build\fontfs.bin
```
