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
#   make test-host     # 主机端单元测试（无需 ESP-IDF）
#   make clean         # 清理
#   make fullclean     # 彻底清理（删 build/ sdkconfig 重新配置）
#
# 覆盖变量示例：
#   make flash PORT=/dev/ttyUSB0 BAUD=921600
#   make fm PORT=/dev/cu.usbmodem1101
#   make -j8 build
#   make build IDF_CMD=/path/to/idf.py   # 手动指定 idf.py 路径
#
# 前置条件：已安装并激活 ESP-IDF v5.5.1+（执行过 export.sh）。
# 本 Makefile 仅在 macOS / Linux 下使用；Windows 用户请直接使用 idf.py。
# =============================================================================

# 目标芯片固定为 ESP32-S3（与 CMakeLists.txt 一致）
TARGET      := esp32s3

# 串口波特率，可用 BAUD=921600 覆盖
BAUD        ?= 460800

# 串口设备：未显式指定时按 macOS / Linux 自动探测
PORT        ?= $(shell ./tools/detect_port.sh 2>/dev/null)

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

# ----------------------------------------------------------------------------
# 解析 idf.py 调用方式（兼容不同 IDF 版本 / 安装方式的激活行为）
#   1. PATH 上的 idf.py（标准 export.sh 激活后通常可用）
#   2. $IDF_PATH/tools/idf.py（IDF 设了 IDF_PATH 但未把 tools 放进 PATH 时）
# 用户也可显式覆盖：make ... IDF_CMD=/path/to/idf.py
# ----------------------------------------------------------------------------
idf_by_path := $(shell command -v idf.py 2>/dev/null)
idf_by_env  := $(if $(IDF_PATH),$(wildcard $(IDF_PATH)/tools/idf.py))
ifeq ($(idf_by_path),)
IDF_CMD ?= $(idf_by_env)
else
IDF_CMD ?= $(idf_by_path)
endif

# 检查 idf.py 是否可用（给出最详细的指引）
define check_idf
	@if [ -z "$(IDF_CMD)" ]; then \
		printf "$(COLOR_RED)[ERROR] 未找到 idf.py。请先安装并激活 ESP-IDF v5.5.1+：$(COLOR_RESET)\n"; \
		printf "    . \"$$HOME/.espressif/*/esp-idf/export.sh\"   (新版安装器)\n"; \
		printf "    . \"$$HOME/esp/esp-idf/export.sh\"            (经典安装)\n"; \
		printf "  或手动指定：make ... IDF_CMD=/path/to/idf.py\n"; \
		printf "  出错退出。\n"; \
		exit 1; \
	fi
endef

# 默认目标
.DEFAULT_GOAL := help

.PHONY: help setup build flash monitor fm menuconfig size clean fullclean reconfig erase-flash test test-host test-clean tools tools-fonts tools-font tools-city

help: ## 显示所有可用目标
	@printf "$(COLOR_BOLD)epaper_uploader 构建命令（封装 idf.py）$(COLOR_RESET)\n"
	@printf "目标芯片：$(COLOR_CYAN)$(TARGET)$(COLOR_RESET)  串口：$(COLOR_CYAN)$(or $(PORT),<自动探测>)$(COLOR_RESET)  波特率：$(COLOR_CYAN)$(BAUD)$(COLOR_RESET)\n\n"
	@printf "$(COLOR_BOLD)常用：$(COLOR_RESET)\n"
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  $(COLOR_GREEN)%-14s$(COLOR_RESET) %s\n", $$1, $$2}' $(MAKEFILE_LIST)
	@printf "\n$(COLOR_BOLD)覆盖变量：$(COLOR_RESET)\n"
	@printf "  PORT=/dev/ttyUSB0     指定串口\n"
	@printf "  BAUD=921600           指定波特率\n"
	@printf "  IDF_BUILD_JOBS=8      并发编译任务数（默认 CPU 核心数 = $(NPROC)）\n"
	@printf "  IDF_CMD=/path/idf.py  手动指定 idf.py 路径（激活异常时用）\n"
	@printf "  SIZES=24                        字库字号（默认 16 24 32；测试可用单字号）\n"

idf-check:
	$(check_idf)

setup: idf-check ## 首次配置目标芯片（esp32s3），只需执行一次
	@printf "$(COLOR_CYAN)[1/1] 设置目标芯片 $(TARGET) ...$(COLOR_RESET)\n"
	$(IDF_CMD) set-target $(TARGET)
	@printf "$(COLOR_GREEN)[OK] 目标已设置为 $(TARGET)。接下来执行：make fm$(COLOR_RESET)\n"

build: idf-check ## 编译固件
	@printf "$(COLOR_CYAN)[build] 编译固件（-j$(IDF_BUILD_JOBS)）...$(COLOR_RESET)\n"
	IDF_BUILD_JOB_COUNT=$(IDF_BUILD_JOBS) $(IDF_CMD) build

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
	$(IDF_CMD) $(PORT_ARG) flash

monitor: idf-check ## 打开串口监视器（自动探测串口）
	@printf "$(COLOR_CYAN)[monitor] 监视 ($(PORT_NOTE)) ...$(COLOR_RESET)\n"
	$(IDF_CMD) $(PORT_ARG) monitor

fm: build ## 编译 + 烧录 + 监视（最常用）
	@printf "$(COLOR_CYAN)[fm] 烧录 + 监视 ($(PORT_NOTE)) ...$(COLOR_RESET)\n"
	$(IDF_CMD) $(PORT_ARG) flash monitor

erase-flash: idf-check ## 擦除整片 Flash（清空所有配置/图片，谨慎使用）
	@printf "$(COLOR_RED)[erase-flash] 将清空整片 Flash（WiFi/配置/图片全丢）！3 秒后开始，Ctrl+C 取消$(COLOR_RESET)\n"
	@sleep 3
	$(IDF_CMD) $(PORT_ARG) erase-flash

menuconfig: idf-check ## 打开 menuconfig 配置菜单
	$(IDF_CMD) menuconfig

size: idf-check ## 查看固件体积 / 分区占用
	$(IDF_CMD) size

clean: idf-check ## 清理构建产物（保留 sdkconfig 与 target 设置）
	$(IDF_CMD) clean

fullclean: idf-check ## 彻底清理（删 build/ 与 sdkconfig，需重新 make setup）
	@printf "$(COLOR_YELL)[fullclean] 删除 build/ 与 sdkconfig ...$(COLOR_RESET)\n"
	$(IDF_CMD) fullclean
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
# 首次使用先执行 `make tools` 安装依赖；之后可直接调用。
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


# 生成的字号列表（测试某种字体时可用 make tools-fonts SIZES=24）
SIZES ?= 16 24 32

tools-fonts: fetch-fonts ## 生成字库，SIZES= 指定字号(默认 16 24 32)
	@printf "$(COLOR_CYAN)[tools-fonts] 生成字库（字号: $(SIZES)）...$(COLOR_RESET)\n"
	$(UV) run python tools/gen_ext_font.py --help >/dev/null 2>&1 || $(MAKE) tools
	@for s in $(SIZES); do \
		printf "$(COLOR_CYAN)  生成 $${s}px...$(COLOR_RESET)\n"; \
		$(UV) run python tools/gen_ext_font.py --size $$s \
			--out spiffs_image/fonts/cjk$$s.mef --set gb2312 \
			--quiet; \
	done
	@printf "$(COLOR_GREEN)[OK] 字库已生成到 spiffs_image/fonts/$(COLOR_RESET)\n"


fetch-fonts: ## 下载字体源文件（Unifont + Fusion Pixel）
	@printf "$(COLOR_CYAN)[fetch-fonts] 下载字体源文件...$(COLOR_RESET)\n"
	$(UV) run python tools/download_fonts.py
	@printf "$(COLOR_GREEN)[OK] 字体源文件已就绪$(COLOR_RESET)\n"
tools-font: tools-fonts ## = tools-fonts 的别名（单字号测试：make tools-font SIZES=24 FONT=fz）

tools-city: ## 导出和风天气城市码 Excel（需先 make tools；openpyxl）
	@printf "$(COLOR_CYAN)[tools-city] 导出城市码 Excel ...$(COLOR_RESET)\n"
	$(UV) run python tools/gen_city_excel.py
