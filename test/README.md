# 单元测试（host 端）

本目录存放**主机端单元测试**，用于在不烧录真机的前提下，对固件中**纯逻辑模块**做回归验证。
测试只新增代码，**不修改 `main/` 下任何现有实现逻辑**。

测试运行在 **ESP-IDF Linux 主机目标** 上，使用 ESP-IDF 自带的 **Unity** 框架。

## 目录结构

```
test/
└── lunar/                  # 针对 main/lunar.c（农历算法）的测试工程
    ├── CMakeLists.txt       # 顶层 IDF 工程
    └── main/
        ├── CMakeLists.txt   # 编译 main/lunar.c + 测试用例，依赖 unity 组件
        └── test_lunar.c     # Unity 测试用例 + 运行入口
```

## 运行方法

在**已激活 ESP-IDF 环境**的终端里执行（需要 ESP-IDF v5.5.1+）：

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
