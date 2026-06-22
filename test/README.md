# 单元测试（host 端）

本目录存放**主机端单元测试**，用于在不烧录真机的前提下，对固件中**纯逻辑模块**做回归验证。
测试只新增代码，**不修改 `main/` 下任何现有实现逻辑**。

测试提供**两条运行路径**：

- **`test/host/`（推荐，无需 ESP-IDF）：** 用系统 `cc` 直接编译纯 C 模块，自带最小 `unity.h` / `sdkconfig.h` 兼容层，复用 `test/lunar/main/test_lunar.c` 的全部用例。`make test-host` 一条命令运行。
- **`test/lunar/`（完整 Unity，需 ESP-IDF）：** 在 ESP-IDF Linux 主机目标上用 ESP-IDF 自带的 Unity 框架编译运行，适合需要完整 Unity 输出格式的场景。

```
test/
├── host/                   # ESP-IDF-free 主机测试（推荐，纯 cc 编译）
│   ├── Makefile            # 构建运行脚本（make run / clean）
│   ├── unity.h             # 最小 Unity 兼容层（仅覆盖 lunar 测试用到的宏）
│   ├── sdkconfig.h         # 最小 sdkconfig 存根（启用 CONFIG_IDF_TARGET_LINUX 路径）
│   └── test_lunar_host.c   # 驱动：引入兼容层 + #include 既有 test_lunar.c + 提供 main()
└── lunar/                  # ESP-IDF Linux 主机目标测试工程（完整 Unity）
    ├── CMakeLists.txt       # 顶层 IDF 工程
    ├── Makefile             # macOS/Linux 快捷封装（run / clean）
    └── main/
        ├── CMakeLists.txt   # 编译 main/lunar.c + 测试用例，依赖 unity 组件
        └── test_lunar.c     # Unity 测试用例 + 运行入口（host/ 与 lunar/ 共用）

## 运行方法

### 方式一：ESP-IDF-free（推荐，`test/host/`）

纯 C 编译，**无需安装/激活 ESP-IDF**，只需系统自带 C 编译器（macOS/Linux 的 `cc`/`clang`/`gcc`）：

```bash
# 在仓库根目录执行
make test-host          # 编译并运行（make test 的默认目标）
# 或单独：make -C test/host run
# 清理：make -C test/host clean
```

测试通过返回码为 `0`，失败为 `1`，可直接接入 CI。

### 方式二：ESP-IDF Linux 主机目标（`test/lunar/`，完整 Unity）

在**已激活 ESP-IDF 环境**的终端里执行（需要 ESP-IDF v5.5.1+）。仓库根目录 `Makefile` 也提供了快捷方式：

```bash
# macOS / Linux 快捷方式（等价于下方完整流程）
make -C test/lunar run
```

**完整命令**（全平台）：

```powershell
cd test/lunar

# 1. 切换到 Linux 主机目标（首次执行，需要 --preview）
idf.py --preview set-target linux

# 2. 编译
idf.py build

# 3. 运行生成的可执行文件
#    Windows（需安装 MinGW；ESP-IDF Tools 通常已附带）：
.\build\lunar_unit_test.elf
#    Linux / macOS：
./build/lunar_unit_test.elf
```

测试通过时进程返回码为 `0`，有失败用例时返回 `1`，便于接入 CI。

> 说明：Linux 主机目标依赖宿主机的 C 编译器。Windows 上若未安装 MinGW，可在 WSL / Linux / macOS 中运行；
> 或改用 `idf.py --preview set-target linux` 配套的工具链。`lunar.c` 为纯 C（仅依赖 `<stdio.h>`/`<string.h>`），
> 不引入任何 ESP-IDF 运行时依赖，因此可干净地在主机端编译。

## 已覆盖范围（`test/lunar`）

针对 `main/lunar.c` 公开 API：

| 测试 | 覆盖函数 | 关键锚点 |
|------|----------|----------|
| 公历→农历转换 | `lunar_from_solar` | 表基准 1900-01-31、2000/2023/2024 春节、2023 闰二月、中秋/端午 |
| 越界处理 | `lunar_from_solar` | 1899 / 2101 / 基准日之前返回 false |
| 日期字符串 | `lunar_day_str` `lunar_month_str` | 初一/十五/廿一/三十、正月/冬月/腊月、越界返回 "" |
| 干支与生肖 | `lunar_year_gz` `lunar_year_sx` | 1984=甲子鼠、2020=庚子鼠、2024=甲辰龙 |
| 24 节气 | `lunar_solar_term` | 2024 清明/冬至、非节气日、2000-2099 范围外 |
| 节日 | `lunar_festival` | 公历元旦/劳动/国庆/妇女/儿童，农历春节/中秋/端午 |

## 如何扩展

后续可为其它**纯逻辑函数**新增同样结构的子工程（例如课程表周次解析、`json_escape` 等）。
每个子工程只需：

1. 在 `main/CMakeLists.txt` 的 `SRCS` 里加入被测 `.c`（用相对路径指向 `../../../main/xxx.c`）。
2. 新增 `test_xxx.c`，编写 `RUN_TEST(...)` 用例。

注意：被测模块若依赖 ESP-IDF 运行时（NVS、FreeRTOS、esp_log 等），需要先评估能否在主机端 mock，
或改用真机 `test_apps` 方案。优先从零依赖的纯算法模块入手。
