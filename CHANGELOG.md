# 更新日志

本文记录公开仓库中对用户和复刻者有影响的主要变化。


## 2026-06-22

### 新增

- 新增 macOS / Linux 跨平台 `Makefile`：封装 `idf.py`，提供 `setup` / `build` / `flash` / `monitor` / `fm` / `test` / `test-host` / `tools` / `tools-fonts` / `tools-city` / `clean` / `fullclean` 等目标，通过 `tools/detect_port.sh` 自动探测串口（macOS `/dev/cu.*`、Linux `/dev/ttyUSB*` / `/dev/ttyACM*`），`make help` 查看全部目标。Windows 用户仍使用 `idf.py` 原生命令。
- 新增 `test/lunar/Makefile` 与 `test/host/Makefile`：`make test`（默认走 ESP-IDF-free 的 `test/host`）一条命令完成主机端单元测试编译与运行。
- 新增 **ESP-IDF-free 开发工具链**：Python 开发工具（字库生成、城市码导出）与主机单元测试不再依赖 ESP-IDF。
  - 新增 `pyproject.toml`（uv 项目声明）与 `uv.lock`（依赖版本锁定），使用 [uv](https://docs.astral.sh/uv/) 管理虚拟环境：`uv sync --group dev` 安装 Pillow / openpyxl，`.venv/` 已被 `.gitignore` 忽略。
  - 新增 `test/host/`（ESP-IDF-free 主机测试）：自带最小 `unity.h` / `sdkconfig.h` 兼容层，用系统 `cc` 直接编译纯 C 的 `main/lunar.c`，复用 `test/lunar/main/test_lunar.c` 全部用例，`make test-host` 运行。
  - `Makefile` 新增 `tools` / `tools-fonts` / `tools-city` 目标，封装 `uv run` 调用字库与城市码工具。
  - 固件编译 / 烧录仍需 ESP-IDF v5.5.1+；文档已明确区分两类任务的依赖边界。

### 文档

- README、AGENTS、CONTRIBUTING、test/README 同步补充 macOS / Linux 构建说明、Makefile 用法，以及基于 uv 的 ESP-IDF-free 开发工具链说明。

## 2026-06-04

### 新增

- 新增 Codex/中转站额度看板：配置页支持填写额度 API URL、API Key、显示单位和自动刷新间隔，墨水屏可显示余额、已用额度、今日用量、请求数与令牌数。
- 新增 `/codex_quota_config` 与 `/codex_quota_show` HTTP 接口，并将额度页纳入按键显示模式、配置备份/恢复和深睡模式记忆。

### 修复与发布

- 将 ESP-IDF main task 栈从默认值提高到 8192 字节，避免启动欢迎页切换到时钟页时出现 `task main` 栈溢出重启。
- 重新生成 release 整包固件与校验信息，确保 Gitee 免编译烧录包与当前源码一致。

## 2026-05-09

### 修复

- 修复时钟模式在 `clock.enabled=false` 但当前屏幕已处于时钟时不继续刷新的问题；SNTP 同步成功后会触发一次重绘，后续分钟刷新也会正常执行。
- 调整天气与时钟联动：当前为时钟模式时，天气后台刷新只通知时钟摘要更新，不再抢占成全屏天气页。
- 优化 SPIFFS 恢复流程：格式化成功后网页能拿到明确 JSON 结果并提示即将重启，设备随后自动重启离开 recovery 页面；文件系统不可用时 `/images` 返回空列表和 `spiffs_ok:false`，避免图库直接 500。
- 修复 4.2" 小屏待办显示拥挤问题：按实际高度限制条数，优先显示紧急、重要和未完成事项，页脚显示已显示/总数。
- 修复日历模式今日红圈遮挡农历、节日和节气文字的问题，改为空心圈高亮。
- 优化 SETUP 欢迎页 WiFi 信息显示，4.2" 双栏下使用 `STA/IP/AP/PW/WEB/MD` 短标签，避免热点、密码和访问地址被截断。

### 字库与显示

- 文本渲染层将全角英文、全角数字和全角常见符号映射为已有 ASCII 字形，改善网页输入法产生全角字符时的显示。
- 调整 ASCII 字体生成来源并增加校验，确保 `W/w/A-Z/a-z/0-9` 不是空白或严重裁切。
- 新增 `tools/font_extra_chars.txt` 作为小规模补字清单；后续遇到缺字时可追加字符并重新生成 `main/font_data.h`。
- 非 ASCII 字符缺字时显示占位框，不再完全空白，便于实机发现需要补充的字符。

### 工程整理

- 新增 `main/ui_theme.c` / `main/ui_theme.h`，统一墨水屏页面框架、标题栏、卡片、页脚和常用 UI 绘制。
- 将本地字体缓存目录 `tools/fonts/` 视为生成辅助资源，不随仓库提交；固件内置字库仍由 `main/font_data.h` 提供。

## 2026-05-02

### 新增

- 画布留言板增加图片预处理参数：阈值、对比度、红色敏感度、抖动强度、反色、适应/裁剪/拉伸。
- 画布图片参数支持浏览器会话内实时预览，便于提前查看黑白红墨水屏效果。
- 画布属性面板改为分组折叠，并增强图层列表、选中框、控制点、对齐辅助线和发送前确认预览。
- 新增 Gitee 二维码名片模板，默认指向 `https://gitee.com/gxp666111/miaomiao`。
- 新增 `CONTRIBUTING.md`、Gitee Issue 模板、PR 模板和 Release 草案。
- README 和工程文档补充微雪移植面板支持状态，区分主测、已验证和待实机验证。

### 修复

- 修复无 PSRAM 或内存碎片场景下部分图片转换失败的问题；在有 PSRAM 的 ESP32-S3 上优先使用 PSRAM 承载大块图片/画布数据。
- 修复画布渲染图片素材时长时间占用 CPU 导致任务看门狗报警的问题。
- 修复二维码模板在小屏上模块过小、矩阵不可靠导致手机扫码失败的问题。
- 修复 README 中 API 章节锚点在 Gitee 上无法正确跳转的问题。
- 天气请求失败日志对 URL 中的 `key=` 参数做脱敏，降低 API Key 泄露风险。
- 固定新下载源码的默认 ESP-IDF target 为 `esp32s3`，并在误设为 `esp32` 时给出明确 CMake 错误，避免 `GPIO_NUM_46` / USB Serial JTAG 头文件误报。
- 7.5 寸三色屏选项对齐 GDEW075Z08 / UC8179，并接入参考 FastFreshBWOnColor 的黑白快刷路径。

### 仓库整理

- 从公开版本库移除 Waveshare 参考包、Android App 示例源码和重复原始素材目录。
- 公开仓库保留核心固件、Web 前端、工程文档、硬件提要和 `docs/images` 展示素材。
- 将 ESP-IDF 组件清单中的最低版本约束同步为 `>=5.5.1`，与 README 和当前工程要求一致。

## 2026-04-28

### 稳定性整理

- 完成显示 epoch 仲裁，降低旧显示任务覆盖新操作的概率。
- 日历、画布、留言等异步显示接口改为等待渲染结束或返回 `canceled=true`。
- 补齐数据/媒体类 GET 接口的 Basic Auth 覆盖。
- SPIFFS 挂载失败时不再自动格式化，保留 AP/Web 恢复入口。
- 面板配置保存后提示重启，避免热切换期间 framebuffer 和缓存尺寸不一致。
- 配置备份/恢复不包含 Basic Auth 密码。
- EPD 初始化失败时进入降级诊断路径，尽量保留 Web 恢复能力。

## 2026-04-26

### 屏幕与刷新

- 4.2" SSD1619 增加局部刷新基础路径，但默认优先稳定全刷。
- 5.83" UC8179 实机确认局部刷新不稳定，默认禁用 partial 路径。
- 时钟任务避免每分钟全屏刷新，改为按配置和时间边界控制刷新节奏。

## 2026-03 至 2026-04

### 功能主线

- 完成图片上传、图库、轮播、天气、时钟、日历、课程表、待办、倒数日、留言板、画布、OTA 和低功耗基础能力。
- 支持 SoftAP + STA 配网、mDNS、SNTP、WiFi 自动重连和 Web 配置。
- 支持 4.2" 400x300 与 5.83" 648x480 三色墨水屏配置。
