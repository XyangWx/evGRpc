# test_consumption.md

## 概述
- **服务：** ConsumptionService
- **RPC：** CreateConsumption, GetConsumption, UpdateConsumption, DeleteConsumption, ListConsumptions
- **测试总数：** 21（原 20 个；Phase D 为 page_size INT_MAX 修复新增 1 个）
- **FK 依赖：** vehicle_id（必填，NOT NULL）；weather_id（DB 层允许 NULL，
  但生产代码要求非空 —— 见下面的 "Constraints"）。

## TestHappyPath

### test_create_consumption_returns_id_and_fields
- **RPC：** CreateConsumption
- **辅助：** `vehicle_and_weather` fixture 在一个 with-block 中创建两个 FK 引用。

### test_get_consumption_returns_created
- **RPC：** GetConsumption
- **`with` 内部** —— 同样的清理理由。

### test_update_consumption_changes_end_percent
- **RPC：** UpdateConsumption

### test_delete_consumption_removes_row
- **RPC：** DeleteConsumption
- **Delete 后调用 `ti.unregister`**。

### test_list_consumptions_after_create_includes_new
- **RPC：** ListConsumptions
- 按 vehicle_id 过滤；新的 consumption 出现在列表中。

## TestErrorPath

### test_get_consumption_unknown_id_returns_not_found
### test_update_consumption_unknown_id_returns_not_found
### test_delete_consumption_unknown_id_returns_not_found
- 三者一致：随机 UUID → NOT_FOUND。

## TestBoundaries

### test_create_consumption_end_equal_start_returns_invalid
- **App 校验：** end > start。

### test_create_consumption_end_percent_geq_begin_returns_invalid
- **App 校验：** end_percent < begin_percent（consumption 是耗电过程）。

### test_create_consumption_highest_lt_lowest_temp_returns_invalid
- **App 校验：** highest_temperature_c >= lowest_temperature_c。

### test_create_consumption_highest_eq_lowest_temp_ok
- **App 校验边界：** highest == lowest 是允许的（用的是 `>=` 而不是 `>`）。

### test_create_consumption_negative_begin_percent_accepted
- **记录当前行为：** 负百分比可被接受（无 app 校验）。

### test_create_consumption_negative_temperature_accepted
- **记录当前行为：** 负摄氏温度可被接受。

### test_create_consumption_early_ev_year_accepted
- **记录当前行为：** TIMESTAMPTZ 允许任意年份（1990）。

## TestConstraints

### test_create_consumption_invalid_vehicle_id_returns_invalid_argument
- **FK 违反**（vehicle.Id 不存在）→ INVALID_ARGUMENT。

### test_create_consumption_invalid_weather_id_returns_invalid_argument
- **FK 违反**（weather.Id 不存在）→ INVALID_ARGUMENT。

### test_create_consumption_empty_weather_id_returns_invalid
- **App 校验（Phase 3 修复）：** `ValidateConsumption` 现在以 INVALID_ARGUMENT
  拒绝空的 weather_id。WeatherId 在 DB schema 里是 NOT NULL，所以空串
  原本会撞到 NOT NULL 约束，再走 `error.cc` 里的 `not_null_violation` → INTERNAL
  fallback。

### test_update_consumption_empty_weather_id_returns_invalid
- **UpdateConsumption** 走同一个 `ValidateConsumption`（通过构造的
  CreateConsumptionRequest-shaped view）。加这个测试确认修复在 update
  路径上也生效。


### test_list_consumptions_invalid_page_token_returns_invalid
- **Phase A 修复：** 非数字 page_token → INVALID_ARGUMENT
  （原本是 INTERNAL）。


### test_list_consumptions_int_max_page_size_caps_to_1000
- **Phase D 修复：** page_size=INT_MAX → OK，截到 999。


## 跨服务测试（Chunk 6 启用了 Vehicle FK-delete 测试）

`tests/python/test_vehicle.py::TestConstraints::test_delete_vehicle_with_consumption_returns_invalid_argument`
之前标记为 `@pytest.mark.skip`（等 Chunk 6）。Chunk 6 实现移除了 skip
—— 该测试现在运行，验证：
1. 创建车辆 V
2. 创建天气 W
3. 创建 consumption C，引用 V（和 W 作为 weather_id）
4. 尝试 DeleteVehicle(V) → INVALID_ARGUMENT（FK 违反，
   vehicle.Id 有 consumption 行指向它）
5. 车辆未被删除；清理同时处理两行。

依据 `error.cc`：`foreign_key_violation` → INVALID_ARGUMENT
（round-1 review 修复后的结果）。