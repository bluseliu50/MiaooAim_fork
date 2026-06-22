# 贡献指南

感谢你愿意改进喵喵智能墨水屏项目。这个仓库面向复刻和二次开发，提交问题时请尽量提供可复现信息，提交代码时请保持改动聚焦。

## 环境基线

- 芯片：ESP32-S3，建议 16MB Flash；有 8MB PSRAM 时图片转换体验更好。
- ESP-IDF：最低 `v5.5.1`，建议使用同一 5.5.x 系列。
- 默认分区：`partitions.csv`，包含 factory、双 OTA 和约 10MB SPIFFS。
- 推荐命令：

```powershell
idf.py fullclean
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

如果从 Gitee 下载源码 ZIP 后用 VSCode 打开，先确认没有旧的 `build/`、`sdkconfig`、`managed_components/` 残留；这些目录/文件是本地生成物，不应提交。

## 提交 Issue 前

请先确认：

- 已阅读 `README.md` 的硬件要求、引脚表和首次使用步骤。
- 已在配置页选择正确屏幕型号，并保存后重启。
- 已记录串口日志中从启动到问题出现的关键片段。
- 若是图片/画板问题，请说明图片格式、尺寸、是否启用 PSRAM、使用的预处理参数。

建议 Issue 包含：

- 硬件：开发板型号、Flash/PSRAM、屏幕尺寸、驱动 IC、BUSY 电平。
- 软件：ESP-IDF 版本、固件提交号、浏览器型号。
- 现象：期望结果、实际结果、复现步骤。
- 日志：串口日志、HTTP 错误、截图或屏幕照片。

## 适配新屏幕

屏幕适配 PR 请尽量包含：

- 面板品牌、尺寸、分辨率、颜色类型、驱动 IC。
- FPC 或商品链接中的初始化资料。
- BUSY 空闲电平、刷新耗时、是否支持局部刷新。
- 初始化序列、全刷测试、黑白红平面顺序验证。
- 至少一张实机刷屏照片或串口日志。

涉及文件通常包括：

- `main/epd.h`
- `main/epd_stub.c`
- `web/config.html`
- `README.md` 或工程文档

## Pull Request 要求

- 一个 PR 只解决一类问题，避免把驱动、UI、文档和格式化混在一起。
- 提交前运行 `idf.py build`。
- 前端改动请确认页面语法无误，并尽量在手机浏览器上试一次。
- 不要提交 `build/`、`sdkconfig`、`managed_components/`、本地参考包或个人素材。
- 不要提交真实 WiFi 密码、天气 API Key、私钥、签名证书或个人敏感信息。

## 代码风格

- C 代码保持现有 ESP-IDF 风格，错误路径明确返回 `esp_err_t`。
- 长时间循环要让出 CPU，避免触发任务看门狗。
- 涉及 SPIFFS/NVS 的写入要考虑断电、坏数据和重复写入。
- 涉及 HTTP 接口时要考虑 Basic Auth、输入长度、JSON 校验和错误提示。

## 文档同步

如果改动会影响用户使用方式，请同步更新：

- `README.md`
- `CHANGELOG.md`
- `AGENTS.md`（架构、模块、API、代码约定）
- 对应专题文档，例如 `COURSE_TABLE_README.md`
