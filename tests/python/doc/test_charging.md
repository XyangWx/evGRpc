# test_charging.md

## 概述
- **服务：** ChargingService
- **RPC：** CreateCharging, GetCharging, UpdateCharging, DeleteCharging, ListChargings
- **测试总数：** 25（原 24 个；Phase M 为 UpdateCharging UNSPECIFIED 新增 1 个）
- **FK 依赖：** 每个 Create/Update 都需要合法的 vehicle_id + source_category_id（UUID）。

## TestHappyPath

### test_create_charging_returns_id_and_fields
- **RPC：** CreateCharging
- **目的：** 用合法 vehicle + source_category + 合法字段创建，返回 UUID 并回显字段。
- **辅助：** `_make_charging_req(vehicle_id, source_category_id)` 构造合法请求；
  `vehicle_and_source` fixture 提供 FK 引用。
- **清理：** TrackedInsert。

### test_get_charging_returns_created
- **RPC：** GetCharging
- **目的：** Round-trip Get 必须在 `with` 块内部（避免 Weather chunk 里抓到的
  "清理删行" 问题）。

### test_update_charging_changes_kwh
- **RPC：** UpdateCharging
- **目的：** 更新 kwh_charged；验证响应反映该变更。
- **`with` 内部** —— 同样的清理理由。

### test_delete_charging_removes_row
- **RPC：** DeleteCharging
- **目的：** Delete 移除该行；后续 Get → NOT_FOUND。
- **Delete 后调用 `ti.unregister(created.id)`**（round-1 review 修复）。

### test_list_chargings_after_create_includes_new
- **RPC：** ListChargings
- **目的：** 新创建的 charging 出现在按 vehicle_id 过滤的 List 中。

## TestErrorPath

### test_get_charging_unknown_id_returns_not_found
### test_update_charging_unknown_id_returns_not_found
### test_delete_charging_unknown_id_returns_not_found
- 三者一致：随机 UUID → NOT_FOUND。

## TestBoundaries

### test_create_charging_end_time_equal_start_returns_invalid
- **App 校验**（`charging_service.cc:ValidateCharging`）：end_time > start_time。
- 相等 → INVALID_ARGUMENT。

### test_create_charging_end_percent_equal_start_returns_invalid
- **App 校验：** end_percent > start_percent。

### test_create_charging_kwh_zero_returns_invalid
- **App 校验：** kwh_charged > 0。

### test_create_charging_cost_zero_returns_invalid
- **App 校验：** cost > 0。

### test_create_charging_location_length
- **VARCHAR(100)：** 101 字符的 location → data_exception → INVALID_ARGUMENT。

### test_create_charging_location_at_limit_ok
- **VARCHAR(100)：** 100 字符的 location（恰好到上限）→ OK 且回显长度为 100。

### test_create_charging_negative_kwh_returns_invalid
- **App 校验：** kwh_charged > 0；-1.0 被拒。

### test_create_charging_negative_cost_returns_invalid
- **App 校验：** cost > 0；-50.0 被拒。

### test_create_charging_slow_charger_type_ok
- CHARGER_TYPE_SLOW 是 CHARGER_TYPE_FAST 的合法替代。

### test_create_charging_with_service_fee_ok
- DoubleValue wrapper 设 value=5.0；round-trip OK。

### test_create_charging_no_service_fee_is_null
- DoubleValue 未设置 → 响应 `HasField("service_fee") == False`。

## TestConstraints

### test_create_charging_invalid_vehicle_id_returns_invalid_argument
- **FK 违反**（vehicle.Id 不存在）→ `foreign_key_violation` →
  INVALID_ARGUMENT（依据 `error.cc`）。

### test_create_charging_invalid_source_category_id_returns_invalid_argument
- **FK 违反**（source_category.Id 不存在）→ INVALID_ARGUMENT。

### test_create_charging_charger_type_unspecified_returns_invalid
- **App 校验（Phase 3 修复）：** `ValidateCharging` 以 INVALID_ARGUMENT 拒绝
  UNSPECIFIED。否则 ChargerTypeLabel('') → NOT NULL violation → INTERNAL
  （远不如 INVALID_ARGUMENT 直观）。

### test_update_charging_charger_type_unspecified_returns_invalid
- **Phase M 覆盖补漏：** UpdateCharging 收到 UNSPECIFIED → INVALID_ARGUMENT。
- UpdateCharging 构造一个 CreateChargingRequest-shaped 的 view 然后调用
  ValidateCharging，所以同一条规则也适用。
  防止以后的 refactor 在 update 路径上绕过校验器。


### test_list_chargings_invalid_page_token_returns_invalid
- **Phase A 修复：** 非数字 page_token → INVALID_ARGUMENT
  （原本经 `std::stoi` 抛错被通用 catch 抓住 → 默认 INTERNAL）。

### test_list_chargings_int_max_page_size_caps_to_1000
- **Phase D 修复：** page_size=INT_MAX → OK，截到 999（不是 INT_MAX 溢出）。