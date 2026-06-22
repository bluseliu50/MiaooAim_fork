# 元器件清单（BOM）

## 硬件提示（2026-04-28）

复刻前请确认 Flash 容量、面板型号、BUSY 极性、电源静态电流、按键 RTC 唤醒能力和烧录流程。固件侧已将面板切换改为“保存后重启生效”，关闭 SPIFFS 挂载失败自动格式化，并在正常启动时尽量保留 AP/Web 用于 EPD 初始化失败诊断。

> **说明**：含料号、供应商、价格等**采购级元器件清单暂不随本仓库公开发布**。配套 4.2" SSD1619 PCB/外壳已开源到嘉立创：[喵哎-MiaooAim 4.2寸 墨水屏 SSD1619](https://oshwhub.com/team_voosogmo/project_fxbcjhaa)。复刻请优先阅读根目录 [`README.md`](../README.md) 的**硬件要求**、**引脚连接**与**分区表**。

## 与固件对齐的硬指标（摘录）

- **Flash**：默认 `partitions.csv` 约需 **16MB** 级 Flash（factory + 双 OTA + ~10MB SPIFFS）。**仅 8MB Flash（如部分 N8 模组）无法直接套用当前默认分区**，须改分区后再烧录。
- **PSRAM**：**非必需**；固件按无 PSRAM 做内存优化；亦可选 `N16R8` 等带 PSRAM 料号。
- **墨水屏**：驱动以 **4.2" SSD1619（BWR）**、**5.83" UC8179（BWR）** 为主；引脚默认值以 `README.md` 为准。

更多硬件说明见 [`AGENTS.md`](../AGENTS.md) 与 [`README.md`](../README.md) 的硬件要求章节。
