# test_weather.md

## 概述
- **服务：** WeatherService
- **RPC：** CreateWeather, SearchWeather
- **测试总数：** 14（原 7 个；Phase 2 round-2 review 新增 7 个）
- **目的：** 验证 name 的 create/search 语义 + VARCHAR(36) 长度边界。

## TestHappyPath

### test_create_weather_returns_id_and_name
- **RPC：** WeatherService.CreateWeather
- **目的：** 验证合法的 Create 返回该行的 UUID 和回显的 name。
- **前置：** namespace fixture 提供 session prefix；name 来自 `make_weather_name(ns)`。
- **操作：** 调用 `CreateWeather(name=name)`。
- **预期：** 响应的 `id`（UUID）非空，`name == request.name`。
- **清理：** TrackedInsert 在 `__exit__` 时按 id 删除。
- **关联：** 基础 smoke 测试（`test_smoke.py`）覆盖 SearchWeather；
  本测试是 Create 对应的那一端。

### test_search_weather_finds_created_weather
- **RPC：** WeatherService.SearchWeather
- **目的：** 验证 SearchWeather 能通过 prefix 找到一行（`^@` "starts with"）。
- **前置：** 创建一行，`name = marker + "-" + make_weather_name(ns)`，
  其中 marker 是新生成的 6 字符 hex。总长度 = 36（VARCHAR(36) 上限）。
- **操作：** 在 `with TrackedInsert` 块内部（确保 search 执行时该行还存在）
  调用 `SearchWeather(prefix=marker, limit=5)`。
- **预期：** 至少有一个匹配的 `m.name == name`。
- **原理：** PG 的 `^@` 表示 "starts with"；marker 必须出现在 name 的开头，
  而不是结尾。`-` 分隔符避免了和其他测试的 uuid-suffix name 混淆。
- **清理：** TrackedInsert 删除该行。
- **关联：** 无。

## TestErrorPath

### test_create_weather_duplicate_name_returns_already_exists
- **RPC：** WeatherService.CreateWeather
- **目的：** `weather.Name` 上的 UNIQUE 约束产生 `ALREADY_EXISTS`
  （依据 `error.cc`：`unique_violation` → `ALREADY_EXISTS`）。
- **前置：** 用 name X 创建一行，注册以备清理。
- **操作：** 在同一个 `with` 块内，用同样的 name X 再尝试创建一行。
- **预期：** `grpc.RpcError`，code 为 `ALREADY_EXISTS`。
- **清理：** TrackedInsert 删除第一行；第二次 Create 失败，没有写入。
- **关联：** 同样覆盖 `vehicle.LicensePlate` 的 UNIQUE 约束（Chunk 3）。

### test_create_weather_empty_name_is_accepted
- **记录当前行为：** VARCHAR(36) 接受空串。没有 app 层面的校验。

### test_create_weather_unicode_name_is_accepted
- **记录当前行为：** VARCHAR 是 UTF-8。中文字符也能存。

### test_create_weather_special_chars_name_is_accepted
- **记录当前行为：** 特殊字符（`!@#`）按原样存储。
  参数化 SQL 阻止了注入。

## TestBoundaries

### test_create_weather_name_length
- **参数化 ID：** `[1]`, `[35]`, `[36]`, `[37]`
- **RPC：** WeatherService.CreateWeather
- **目的：** 验证 VARCHAR(36) 的边界行为：
  - `name_len=1`：1 字符可接受；响应回显长度为 1。
  - `name_len=35`：35 字符可接受（刚好低于上限）；长度 35。
  - `name_len=36`：36 字符可接受（恰好到上限）；长度 36。
  - `name_len=37`：37 字符被拒 → `INVALID_ARGUMENT`
    （"value too long for type character varying(36)"）。
- **前置：** `unique = uuid.uuid4().hex`（32 字符）；按需截断或补 `'x'`
  到 `name_len`。
- **操作：** 对每个 `name_len` 调用 CreateWeather(name=name)。
- **预期：** ID 1/35/36 → OK 且长度正确；ID 37 → RpcError INVALID_ARGUMENT。
- **清理：** TrackedInsert 删除接受的那些行；被拒的行没有插入。
- **关联：** 同样的 VARCHAR 长度模式在 `vehicle.LicensePlate`（Chunk 3，
  `plate_len` 1/15/16）和 `vehicle.Brand`（1/36/37）也覆盖过。

### test_search_weather_empty_prefix_returns_all
- **记录当前行为：** PG `^@ ''` 匹配每一行。在 `with` 内 submit search
  以确保该行还存在。

### test_search_weather_limit_zero_uses_default
- **记录当前行为：** limit=0 → 服务端使用默认值 50（不报错）。

### test_search_weather_negative_limit_uses_default
- **记录当前行为：** limit=-1 → 服务端使用默认值 50。

### test_search_weather_no_matches_returns_empty
- 随机的 32 字符 hex 前缀 → 0 个匹配。

## Removed（来自初始计划，实现时发现无效）

### test_create_weather_empty_name_returns_invalid_argument
- **为何删除：** 生产代码（`weather_service.cc`）**没有**校验空串；
  VARCHAR(36) 接受 ""。该测试假设了一个不存在的 app 层面校验。
  加这个测试会反过来强迫生产代码去做校验，超出测试套件的范围。

### test_create_weather_duplicate_name_returns_already_exists（TestConstraints 重复项）
- **为何删除：** 与 `TestErrorPath.test_create_weather_duplicate_name_returns_already_exists`
  完全重复。保留 ErrorPath 里那个（位置更自然 —— "期望 ALREADY_EXISTS"
  本来就更适合放在 ErrorPath）。