# test/lunar 主机端单元测试 Makefile
# 在已激活 ESP-IDF 环境的终端执行：
#   make run      # 配置（首次）+ 编译 + 运行
#   make clean    # 清理
#
# 由顶层 Makefile 的 `make test` 自动调用。
.DEFAULT_GOAL := run

# Linux 主机目标需要 --preview（首次设置）
TARGET := linux
BIN    := build/lunar_unit_test.elf

.PHONY: run build config clean

config: ## 配置 Linux 主机目标（首次执行，需要 --preview）
	idf.py --preview set-target $(TARGET)

build: ## 编译单元测试
	idf.py build

run: ## 编译并运行单元测试（默认目标）
	@if [[ ! -f build/CMakeCache.txt ]]; then \
		printf '\033[36m[lunar] 首次运行，配置 linux 主机目标 ...\033[0m\n'; \
		idf.py --preview set-target $(TARGET); \
	fi
	idf.py build
	@printf '\033[36m[lunar] 运行 %s ...\033[0m\n' "$(BIN)"
	./$(BIN)

clean: ## 清理构建产物
	idf.py fullclean
