# =============================================================================
# epaper_uploader — ESP32-S3 智能墨水屏终端
# macOS / Linux 跨平台构建脚本（封装 idf.py）
#
# 用法：
#   make help          # 查看全部目标
#   make setup         # 首次配置目标芯片（esp32s3），只需一次
#   make build         # 编译固件
#   make flash         # 烧录（自动探测串口，可用 PORT=/dev/ttyUSB0 覆盖）
#   make monitor       # 串口监视
#   make fm            # 编译 + 烧录 + 监视（常用）
#   make test          # 主机端单元测试（test/lunar）
#   make clean         # 清理
#   make fullclean     # 彻底清理（删 build/ sdkconfig 重新配置）
#
# 覆盖变量示例：
#   make flash PORT=/dev/ttyUSB0 BAUD=921600
#   make fm PORT=/dev/cu.usbmodem1101
#   make -j8 build
#
# 前置条件：已安装并激活 ESP-IDF v5.5.1+（执行过 export.sh / export.bat）。
# 本 Makefile 仅在 macOS / Linux 下使用；Windows 用户请直接使用 idf.py。
# =============================================================================

# 目标芯片固定为 ESP32-S3（与 CMakeLists.txt 一致）
TARGET      := esp32s3

# 串口波特率，可用 BAUD=921600 覆盖
BAUD        ?= 460800

# 串口设备：未显式指定时按 macOS / Linux 自动探测
PORT        ?= $(shell ./tools/detect_port.sh 2>/dev/null)

# idf.py 根目录（默认当前目录）
PROJECT_DIR ?= .

# 并发任务数：默认使用全部 CPU 核心，加速编译
NPROC       := $(shell (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null) || echo 8)
IDF_BUILD_JOBS ?= $(NPROC)

# 颜色输出（非 TTY 时自动关闭）
ifeq ($(shell test -t 1 && echo 1),1)
COLOR_RESET := \033[0m
COLOR_BOLD  := \033[1m
COLOR_GREEN := \033[32m
COLOR_CYAN  := \033[36m
COLOR_YELL  := \033[33m
COLOR_RED   := \033[31m
else
COLOR_RESET :=
COLOR_BOLD  :=
COLOR_GREEN :=
COLOR_CYAN  :=
COLOR_YELL  :=
COLOR_RED   :=
endif

# 默认目标
.DEFAULT_GOAL := help

# 检查 idf.py 是否可用
define check_idf
	@command -v idf.py >/dev/null 2>&1 || { \
		printf "$(COLOR_RED)[ERROR] 未找到 idf.py。请先安装并激活 ESP-IDF v5.5.1+：$(COLOR_RESET)\n"; \
		printf "    . $$HOME/esp/esp-idf/export.sh   (macOS/Linux)\n"; \
		printf "    出错退出。\n"; \
		exit 1; \
	}
endef

.PHONY: help setup build flash monitor fm menuconfig size clean fullclean reconfig erase-flash test test-host test-clean tools tools-fonts tools-city

help: ## 显示所有可用目标
	@printf "$(COLOR_BOLD)epaper_uploader 构建命令（封装 idf.py）$(COLOR_RESET)\n"
	@printf "目标芯片：$(COLOR_CYAN)$(TARGET)$(COLOR_RESET)  串口：$(COLOR_CYAN)$(or $(PORT),<自动探测>)$(COLOR_RESET)  波特率：$(COLOR_CYAN)$(BAUD)$(COLOR_RESET)\n\n"
	@printf "$(COLOR_BOLD)常用：$(COLOR_RESET)\n"
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  $(COLOR_GREEN)%-14s$(COLOR_RESET) %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\n$(COLOR_BOLD)覆盖变量：$(COLOR_RESET)\n"
	@printf "  PORT=/dev/ttyUSB0   指定串口\n"
	@printf "  BAUD=921600         指定波特率\n"
	@printf "  IDF_BUILD_JOBS=8    并发编译任务数（默认 CPU 核心数 = $(NPROC)）\n"

idf-check:
	$(check_idf)

setup: idf-check ## 首次配置目标芯片（esp32s3），只需执行一次
	@printf "$(COLOR_CYAN)[1/1] 设置目标芯片 $(TARGET) ...$(COLOR_RESET)\n"
	idf.py set-target $(TARGET)
	@printf "$(COLOR_GREEN)[OK] 目标已设置为 $(TARGET)。接下来执行：make fm$(COLOR_RESET)\n"

build: idf-check ## 编译固件
	@printf "$(COLOR_CYAN)[build] 编译固件（-j$(IDF_BUILD_JOBS)）...$(COLOR_RESET)\n"
	IDF_BUILD_JOB_COUNT=$(IDF_BUILD_JOBS) idf.py build

# 带串口参数的烧录/监视目标：PORT 探测到则用 -p 传入，否则交给 idf.py 自动探测
ifeq ($(PORT),)
PORT_ARG :=
PORT_NOTE := 未探测到串口（可用 PORT=/dev/ttyUSB0 指定）
else
PORT_ARG := -p $(PORT) -b $(BAUD)
PORT_NOTE := $(PORT) @ $(BAUD)
endif

flash: idf-check ## 烧录固件到设备（自动探测串口）
	@printf "$(COLOR_CYAN)[flash] 烧录 ($(PORT_NOTE)) ...$(COLOR_RESET)\n"
	idf.py $(PORT_ARG) flash

monitor: idf-check ## 打开串口监视器（自动探测串口）
	@printf "$(COLOR_CYAN)[monitor] 监视 ($(PORT_NOTE)) ...$(COLOR_RESET)\n"
	idf.py $(PORT_ARG) monitor

fm: build ## 编译 + 烧录 + 监视（最常用）
	@printf "$(COLOR_CYAN)[fm] 烧录 + 监视 ($(PORT_NOTE)) ...$(COLOR_RESET)\n"
	idf.py $(PORT_ARG) flash monitor

erase-flash: idf-check ## 擦除整片 Flash（清空所有配置/图片，谨慎使用）
	@printf "$(COLOR_RED)[erase-flash] 将清空整片 Flash（WiFi/配置/图片全丢）！3 秒后开始，Ctrl+C 取消$(COLOR_RESET)\n"
	@sleep 3
	idf.py $(PORT_ARG) erase-flash

menuconfig: idf-check ## 打开 menuconfig 配置菜单
	idf.py menuconfig

size: idf-check ## 查看固件体积 / 分区占用
	idf.py size

clean: idf-check ## 清理构建产物（保留 sdkconfig 与 target 设置）
	idf.py clean

fullclean: idf-check ## 彻底清理（删 build/ 与 sdkconfig，需重新 make setup）
	@printf "$(COLOR_YELL)[fullclean] 删除 build/ 与 sdkconfig ...$(COLOR_RESET)\n"
	idf.py fullclean
	rm -rf build sdkconfig sdkconfig.old

reconfig: fullclean setup ## 彻底清理并重新配置目标芯片（切换芯片或 sdkconfig 损坏时使用）

test: test-host ## 编译并运行主机端单元测试（无需 ESP-IDF，无需硬件）

test-host: ## 编译并运行主机端单元测试（test/host，纯 C，无需 ESP-IDF）
	@printf "$(COLOR_CYAN)[test-host] 编译并运行 test/host 单元测试（无需 ESP-IDF）...$(COLOR_RESET)\n"
	$(MAKE) -C test/host run

test-clean: ## 清理单元测试构建产物
	$(MAKE) -C test/host clean

# ----------------------------------------------------------------------------
# Python 开发工具（通过 uv 管理虚拟环境，脱离 ESP-IDF）
# 首次使用先执行 `uv sync` 安装依赖；之后可直接 `make tools` 调用。
# ----------------------------------------------------------------------------
UV ?= uv

tools: ## 安装 Python 开发依赖到 .venv（uv sync --group dev）
	@printf "$(COLOR_CYAN)[tools] uv sync 安装开发依赖 ...$(COLOR_RESET)\n"
	@if ! command -v $(UV) >/dev/null 2>&1; then \
		printf "$(COLOR_RED)[ERROR] 未找到 uv。安装：curl -LsSf https://astral.sh/uv/install.sh | sh$(COLOR_RESET)\n"; \
		exit 1; \
	fi
	$(UV) sync --group dev
	@printf "$(COLOR_GREEN)[OK] 依赖已就绪。用 'uv run python <script>' 运行工具。$(COLOR_RESET)\n"

tools-fonts: ## 生成内置 16/24/32px 字库（需先 make tools；Pillow）
	@printf "$(COLOR_CYAN)[tools-fonts] 生成字库（调用 gen_ext_font.py）...$(COLOR_RESET)\n"
	$(UV) run python tools/gen_ext_font.py --help >/dev/null 2>&1 || $(MAKE) tools
	cd .. && $(UV) run python tools/fetch_open_fonts.py --out-dir tools/fonts/noto
	cd .. && $(UV) run python tools/gen_ext_font.py --size 16 --out spiffs_image/fonts/cjk16.mef --set gb2312 --require-open-fonts --threshold 104 --cjk-stroke 0 --quiet
	cd .. && $(UV) run python tools/gen_ext_font.py --size 24 --out spiffs_image/fonts/cjk24.mef --set gb2312 --require-open-fonts --threshold 110 --cjk-stroke 0 --quiet
	cd .. && $(UV) run python tools/gen_ext_font.py --size 32 --out spiffs_image/fonts/cjk32.mef --set gb2312 --require-open-fonts --threshold 118 --cjk-stroke 0 --quiet
	@printf "$(COLOR_GREEN)[OK] 字库已生成到 spiffs_image/fonts/$(COLOR_RESET)\n"

tools-city: ## 导出和风天气城市码 Excel（需先 make tools；openpyxl）
	@printf "$(COLOR_CYAN)[tools-city] 导出城市码 Excel ...$(COLOR_RESET)\n"
	$(UV) run python tools/gen_city_excel.py
