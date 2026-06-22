# 课程表功能使用说明

## 当前状态（2026-05-02）

课程表功能已接入多模式显示、网页配置、Basic Auth 和 display epoch 仲裁。启用认证后，`GET /timetable.json`、`POST /timetable`、`POST /timetable_show` 均需授权；手动显示课程表时会参与全局显示仲裁，避免旧显示任务覆盖用户新操作。

> **HTTP 鉴权**：若设备上已配置 Basic Auth，则 **`GET /timetable.json`、`POST /timetable`、`POST /timetable_show`** 都需在请求头携带 `Authorization: Basic …`（浏览器在配置页保存认证后，`timetable.html` 会自动附带）。

## 功能概述

课程表功能支持在墨水屏上显示每周课程安排：

- **7×12 网格模型** — 7 天（周一至周日）× 12 节课
- **学期周次追踪** — 设置学期开始日期，自动计算当前周次
- **周次位掩码** — 每门课可指定上课周次（如 "1-5,7,9-12"）
- **自定义节次时间** — 可定义每节课的上下课时间
- **当前/下节课高亮** — 自动高亮当前正在上的课
- **双屏自适应** — 5.83" 显示全周网格，4.2" 显示单日议程

## Web 界面

浏览器打开 `http://设备IP/timetable` 进入课程表编辑页面：

1. **总开关** — 启用/关闭课程表显示
2. **学期开始日期** — 用于计算当前周次
3. **显示天数** — 5 天（工作日）或 7 天
4. **节次时间定义** — 自定义每节课时间，内置标准 10 节模板
5. **课程网格** — 点击单元格编辑课程
6. **导出/导入** — JSON 格式备份恢复
7. **推送到屏幕** — 选择某天或"今天"显示

## API 接口

### 1. 获取课程表配置

```
GET /timetable.json
```

> 注意：`GET /timetable` 返回的是编辑页面（HTML），JSON 数据请用 `/timetable.json`。

**响应示例：**
```json
{
  "enabled": true,
  "period_count": 10,
  "show_days": 5,
  "semester_start": 1743465600,
  "current_week": 8,
  "periods": [
    {"start_hour": 8, "start_minute": 0, "end_hour": 8, "end_minute": 45},
    {"start_hour": 8, "start_minute": 55, "end_hour": 9, "end_minute": 40},
    {"start_hour": 10, "start_minute": 0, "end_hour": 10, "end_minute": 45}
  ],
  "grid": [
    [
      {"name": "高等数学", "room": "A101", "weeks": "1-16"},
      null,
      {"name": "大学英语", "room": "B203", "weeks": "1,3,5,7-9"}
    ],
    [null, null, null],
    [null, null, null],
    [null, null, null],
    [null, null, null],
    [null, null, null],
    [null, null, null]
  ]
}
```

### 2. 设置课程表配置

```
POST /timetable
Content-Type: application/json
```

请求体与 GET 返回 JSON 同类（`grid` 中 `weeks` 为 **字符串**，如 `"1-16"`，与固件内部位掩码互转）。**若启用 Basic Auth，本接口需鉴权。**

### 3. 显示课程表

```
POST /timetable_show
Content-Type: application/json（可选）
```

**请求体（可选）** JSON 字段：

- `day`：整数 `0..6`，**0=周一，6=周日**；省略或无效则按固件逻辑显示「今天」对应日程。

注意：**不是** Query 参数；以下为正确示例：

```json
{"day": 0}
```

**响应：**
```json
{"ok": true}
```

## 数据结构

```c
#define TT_MAX_PERIODS   12    // 最多 12 节课
#define TT_DAYS          7     // 周一到周日
#define TT_NAME_LEN      40    // 课程名字节缓冲（含 '\0'，有效约 39 字）
#define TT_ROOM_LEN      20    // 教室字节缓冲（含 '\0'，有效约 19 字）
#define TT_MAX_WEEKS     25    // 最多 25 周

typedef struct {
    uint8_t start_hour, start_minute;
    uint8_t end_hour, end_minute;
} tt_period_def_t;              // 节次时间定义

typedef struct {
    char     name[TT_NAME_LEN]; // 课程名
    char     room[TT_ROOM_LEN]; // 教室
    uint32_t weeks;             // 周次位掩码: bit0=第1周 …（JSON 中为 weeks 字符串）
} tt_cell_t;                    // 课格

typedef struct {
    bool            enabled;
    uint8_t         period_count;    // 1..12
    uint8_t         show_days;       // 5(工作日) 或 7(含周末)
    int32_t         semester_start;  // 学期起始 Unix 时间戳
    tt_period_def_t periods[TT_MAX_PERIODS];
    tt_cell_t       grid[TT_DAYS][TT_MAX_PERIODS]; // 7天 x 12节
} timetable_config_t;
```

## 周次格式说明

Web 端使用字符串格式（如 `"1-5,7,9-12"`），固件内部转换为位掩码：

| 格式 | 含义 | 位掩码示例 |
|------|------|-----------|
| `1-16` | 第 1 到 16 周 | `0x1FFFF` |
| `1,3,5` | 第 1、3、5 周 | `0x15` (bit0,2,4) |
| `1-5,7,9-12` | 第 1-5、7、9-12 周 | 组合 |
| 空 / null | 所有周都上 | `0x1FFFFFF` |

## 显示效果

### 5.83" 屏幕（648×480）
- 全周网格视图
- 每天一列，每节一行
- 当前节次红色高亮
- 显示课程名和教室

### 4.2" 屏幕（400×300）
- 单日议程视图
- 顶部标题栏：课程表 · 周X
- 当前课程：红色背景白字，显示剩余时间
- 下一节课：简要提示
- 今日课程列表：课程名 + 时间范围

## 使用 curl 操作

```bash
# 获取课程表 JSON

curl -u "user:pass" http://192.168.4.1/timetable.json

# 若未启用 Basic Auth，可省略 -u；启用后 GET/POST 都要带凭据

curl -u "user:pass" -X POST http://192.168.4.1/timetable \
  -H "Content-Type: application/json" \
  -d @timetable_example.json

# 显示今天的课程表（无 body）

curl -u "user:pass" -X POST http://192.168.4.1/timetable_show

# 显示周一（day=0）

curl -u "user:pass" -X POST http://192.168.4.1/timetable_show \
  -H "Content-Type: application/json" \
  -d "{\"day\":0}"
```

## 注意事项

1. 课程表数据保存在 NVS 中，断电不会丢失
2. 需要先同步时间（WiFi + SNTP）才能正确显示当前课程
3. 学期开始日期用于计算当前周次，影响周次位掩码过滤
4. 课程时间不建议重叠（系统不做冲突检查）
5. 课程名最多 39 字符，教室最多 19 字符
