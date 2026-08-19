# test_vehicle.md

## 概述
- **服务：** VehicleService
- **RPC：** CreateVehicle, GetVehicle, UpdateVehicle, DeleteVehicle, ListVehicles
- **测试总数：** 40（原 37 个；Phase D 为 page_size INT_MAX 修复新增 3 个）
- **目的：** CRUD + 边界 + UNIQUE 约束。

## TestHappyPath

### test_create_vehicle_returns_id_and_brand
- **RPC：** VehicleService.CreateVehicle
- **目的：** 验证 Create 返回 UUID 和回显字段。
- **操作：** 用合法字段调用 CreateVehicle。
- **预期：** `id` 非空，`brand == req.brand`。
- **清理：** TrackedInsert。

### test_get_vehicle_returns_created
- **RPC：** VehicleService.GetVehicle
- **目的：** Round-trip Get 返回与 Create 插入的那一行相同的数据。
- **操作：** CreateVehicle + GetVehicle（**两者都在 `with TrackedInsert` 内**
  —— `__exit__` 之后 GetVehicle 会 404，因为清理已经把它删了）。
- **预期：** `got.id == created.id`，`got.license_plate == req.license_plate`。
- **关联：** 与 Weather chunk 里抓到的 "search-after-cleanup" bug 同类；
  记录在这里方便实现者参考。

### test_update_vehicle_changes_brand
- **RPC：** VehicleService.UpdateVehicle
- **目的：** UpdateVehicle 持久化变更。
- **操作：** Create + Update（都在 `with` 内）。
- **预期：** `updated.brand == "test-brand-updated"`。
- **清理：** TrackedInsert。

### test_delete_vehicle_removes_row
- **RPC：** VehicleService.DeleteVehicle
- **目的：** DeleteVehicle 移除该行；后续 Get → NOT_FOUND。
- **操作：** Create + DeleteVehicle + GetVehicle(404) —— 全在 `with` 内。
- **注意：** DeleteVehicle 之后调用 `ti.unregister(created.id)`
  （round-1 review 修复），跳过 L1 清理。
- **预期：** GetVehicle 抛 `NOT_FOUND`。

### test_list_vehicles_returns_response
- **RPC：** VehicleService.ListVehicles
- **目的：** ListVehicles 接受 `page_size` + `page_token`
  （**不是 `page`** —— 计划初稿里字段名写错了）。
- **操作：** ListVehicles(page_size=1)。
- **预期：** 返回 `ListVehiclesResponse`，含可迭代的 `vehicles`
  （可能为空）。
- **关联：** 分页用的是 page_token（offset 字符串），不是页码。

### test_list_vehicles_after_create_includes_new
- **RPC：** VehicleService.ListVehicles
- **目的：** 新创建的车辆可通过分页列表拿到。
- **操作：** Create + 翻页（最多 10 页，page_size=100，page_token=offset）。
- **预期：** 在某一页中找到。清理在 `with` 内（与 GetVehicle 同类 bug）。

## TestErrorPath

### test_get_vehicle_unknown_id_returns_not_found
- **RPC：** VehicleService.GetVehicle
- **目的：** 随机 UUID → NOT_FOUND。

### test_update_vehicle_unknown_id_returns_not_found
- **RPC：** VehicleService.UpdateVehicle
- **目的：** 随机 UUID + 合法的 UpdateVehicleRequest → NOT_FOUND。

### test_delete_vehicle_unknown_id_returns_not_found
- **RPC：** VehicleService.DeleteVehicle
- **目的：** 随机 UUID → NOT_FOUND。

## TestBoundaries

### test_create_vehicle_license_plate_length
- **参数化 ID：** `[1-True]`, `[15-True]`, `[16-False]`
- **VARCHAR(15)：** 1 = OK，15 = 恰好到上限 OK，16 = INVALID_ARGUMENT
  （"value too long"）。
- **辅助：** `_make_create_req(ns, plate_len)` 截断或补 `'x'`。

### test_create_vehicle_brand_length
- **参数化 ID：** `[1-True]`, `[36-True]`, `[37-False]`
- **VARCHAR(36)：** 1 = OK，36 = 恰好到上限 OK，37 = INVALID_ARGUMENT。

### test_create_vehicle_calibrated_range
- **参数化 ID：** `[0-True]`, `[1-True]`, `[2147483647-True]`
- **INT 列：** 0 = OK（无校验），正值 = OK，INT32 上限（2^31-1）= OK。
- **未覆盖：** INT32 溢出（2^31）。Protobuf 的 int32 wire format
  在客户端就拒绝（`ValueError: Value out of range: 2147483648`），
  根本发不出去。PG 的 INT 列接受任意 int4 值；这个边界只存在于
  protobuf 层。

### test_create_vehicle_battery_capacity
- **参数化 ID：** `[0.0-True]`, `[0.01-True]`, `[99999999.99-True]`, `[100000000.0-False]`
- **DECIMAL(10,2)：** 0 = OK（无校验），小值 = OK，上限（99999999.99）= OK，
  溢出 = INVALID_ARGUMENT。

### test_create_vehicle_negative_battery_accepted
- **记录当前行为：** 负 battery 可被接受（无 app 校验）。

### test_create_vehicle_negative_range_accepted
- **记录当前行为：** 负 range 可被接受。

### test_create_vehicle_empty_brand_accepted
- **记录当前行为：** 空 brand 可被接受。

### test_create_vehicle_unicode_brand_accepted
- **记录当前行为：** 中文 brand name 可被接受（UTF-8）。

### test_create_vehicle_earliest_purchase_date_accepted
- **记录当前行为：** DATE 1900-01-01 可被接受（无下限）。

### test_create_vehicle_future_purchase_date_accepted
- **记录当前行为：** 未来的 DATE（2099）可被接受（无上限）。

### test_update_vehicle_change_license_plate
- **RPC：** UpdateVehicle 配新的 license_plate（假设不重复）。

### test_update_vehicle_change_battery_capacity
- **RPC：** UpdateVehicle 配新的 battery_capacity_kwh。

### test_list_vehicles_paging_returns_consistent
- **RPC：** 创建 3 辆车，以 page_size=1 翻页，验证 3 辆都被找到。
- **`with` 块内部** —— 读 RPC 的同种模式。

## TestConstraints

### test_create_vehicle_duplicate_license_plate_returns_already_exists
- `vehicle.LicensePlate` 上的 **UNIQUE 约束**。
- 依据 `error.cc`：`unique_violation` → `ALREADY_EXISTS`。

### test_update_vehicle_to_duplicate_license_plate_returns_already_exists
- 用 V1 的车牌 update V2 → 命中 UNIQUE。
- 同样是 `ALREADY_EXISTS`。

### test_delete_vehicle_with_consumption_returns_invalid_argument
- **状态：** 已跳过（将在 Chunk 6 中 ConsumptionService 测试落地后启用）。
- **FK 约束：** Consumption.VehicleId REFERENCES vehicle(Id)。
  PG 默认 NO ACTION。
- 依据 `error.cc`：`foreign_key_violation` → INVALID_ARGUMENT
  （**不是** FAILED_PRECONDITION —— round-1 review 修复后的结果）。
- **行动计划：** Chunk 6 Step 3 移除 `@pytest.mark.skip` 装饰器。



### test_list_vehicles_invalid_page_token_returns_invalid
- **Phase A 修复：** 非数字 page_token → INVALID_ARGUMENT（原本是 INTERNAL）。
- `std::stoi` 收到非数字时抛 `std::invalid_argument`，
  落到 `error.cc` 的默认 INTERNAL。现在改用 `ParsePageToken` 辅助函数。

### test_list_vehicles_overflow_page_token_returns_invalid
- page_token > INT_MAX → INVALID_ARGUMENT
  （原本经 `std::out_of_range` 报 INTERNAL）。

### test_list_vehicles_empty_page_token_works
- 空 token = 第一页（offset 0）。记录当前行为。



### test_list_vehicles_int_max_page_size_caps_to_1000
- **Phase D 修复：** page_size=INT_MAX（2147483647）→ OK，截到 999
  （不是 INT_MAX 溢出）。
- Bug：page_size + 1 溢出到 INT_MIN，PG 报
  'LIMIT must not be negative'。
- 修复：在 +1 之前把 page_size 截到 kMaxPageSize-1=999。

### test_list_vehicles_negative_page_size_uses_default
- page_size <= 0 → 服务端使用默认值 50。记录当前行为。

## Removed（来自初始计划，实现时发现无效）

### test_create_vehicle_empty_brand_returns_invalid_argument
- **为何删除：** 没有 app 校验；VARCHAR(36) 接受 ""。

### test_create_vehicle_negative_battery_returns_invalid_argument
- **为何删除：** 没有 app 校验；DECIMAL(10,2) 接受负值。

### test_create_vehicle_missing_required_field_returns_invalid_argument
- **为何删除：** proto3 给省略的标量字段填默认值。省略 `purchase_date`
  会发 1970-01-01T00:00:00Z，而不是 "missing"。测试在 proto3 下
  无法区分 "missing" 和 "default"。

### test_create_vehicle_calibrated_range[0-False]
- **为何修改：** 生产代码对 0 没有校验（无 CHECK 约束，无 app validator）。
  0 被接受。

### test_create_vehicle_battery_capacity[0.0-False]
- **为何修改：** 同上 —— 0.0 被接受。