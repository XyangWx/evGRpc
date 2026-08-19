# test_display.md

## 概述
- **服务：** DisplayService
- **RPC：** 11 个（v1.1.0 的 3 个 charging 报表 + 其他 8 个）
- **测试总数：** 42（原 41 个；Phase Q 新增 1 个针对显式 1970-01-01 过滤的回归）
- **策略：** 重点放在校验路径 + v1.1.0 RPC 的空数据响应
  （COALESCE-on-empty 返回 0）。旧 RPC 在空数据时触发 INTERNAL —— 已被记录，
  不强行 seed 数据绕过。

## TestHappyPath (v1.1.0 charging 报表 + VehicleCostSummary)

### test_get_daily_charging_report_empty_returns_zeros
- v1.1.0 RPC，COALESCE(SUM, 0) → year=2024, month=6, day=15，各项总计为 0。

### test_get_monthly_charging_report_empty_returns_zeros
- v1.1.0 RPC → 全部 0。

### test_get_annual_charging_report_empty_returns_zeros
- v1.1.0 RPC → 全部 0，month=0（年度标记）。

### test_get_vehicle_cost_summary_no_data_returns_invalid
- **Phase 3 修复：** EXISTS 预检查在过滤匹配 0 行时，
  现在返回 INVALID_ARGUMENT（"no aggregate row"），而不是 INTERNAL。
  v1.1.0 RPC（Daily/Monthly/AnnualChargingReport）用 COALESCE-on-empty-set
  处理这种情况并返回 0；旧版 GetVehicleCostSummary 选择明确报
  INVALID_ARGUMENT，而不是静默返回 0。

### test_get_vehicle_cost_summary_empty_vehicle_id_returns_invalid
- 校验器：vehicle_id 必填 → INVALID_ARGUMENT。

## TestHappyPathWithSeed

### test_get_vehicle_cost_summary_with_seeded_data_returns_totals
- **RPC：** GetVehicleCostSummary，使用真实 seed 数据
- **目的：** 验证聚合管道（PG → C++ → proto）在数据返回到响应时没有丢数据。
- **前置：** 创建 1 辆车 + 1 条天气 + 1 个 source_category +
  2 条 charging 行 + 1 条 consumption 行，使用已知值
  （kwh=30+40=70，cost=35+50=85，里程增量=100km）。
- **操作：** 用 vehicle_id 和覆盖所有 seed 数据的日期区间调用
  GetVehicleCostSummary。
- **预期：** total_kwh=70，total_cost=85，avg_yuan_per_kwh=85/70≈1.214，
  avg_yuan_per_km=85/100=0.85。
- **关键实现注意：** 查询必须**同时**在 `with TrackedInsert(conn, "charging")`
  和 `with TrackedInsert(conn, "consumption")` 块内部执行，
  这样任何一边的 `__exit__` 都不会在查询读取之前把行清掉。
  consumption 块嵌套在 charging 块内，这样查询时它们都还活着。

### test_get_monthly_charging_report_with_seeded_data_returns_totals
- 2024-06-15 和 16 两天各有 1 条 charging；查 month=2024-06 →
  count=2，kwh=60，cost=70。

### test_get_annual_charging_report_with_seeded_data_returns_totals
- 3 条 charging 跨 2024 Q2/Q3；查 year=2024 → count=3，kwh=60，cost=75。

### test_get_daily_charging_report_with_seeded_data_returns_count
- 2024-06-15 一天有 2 条 charging（不同小时）；查 day=2024-06-15 →
  count=2，kwh=60。

### test_get_cost_by_charger_type_with_seeded_data_returns_breakdown
- 2 条 FAST charging；查询 → 1 个 breakdown（FAST），汇总相应总计。

### test_get_cost_by_source_category_with_seeded_data_returns_breakdown
- 2 条 charging 在同一 source_category；查询 → 1 个 breakdown，
  含汇总总计 + source_category_id 匹配。

### test_get_consumption_efficiency_with_seeded_data_returns_efficiency
- 2 条 charging（共 60 kwh）+ 1 条 consumption（100 km）；
  查询 → km/kwh ≈ 1.667，kwh/100km ≈ 60。

### test_get_range_accuracy_with_seeded_data_returns_accuracy
- 1 条 consumption：begin_range_km=200，end_range_km=100
  （仪表盘 = 100km），end_mileage_km - begin_mileage_km = 200km（实际）。
  查询 → accuracy_ratio = 200/100 = 2.0。

### test_get_temperature_consumption_correlation_with_seeded_data_returns_buckets
- 2 条 consumption，平均温度不同（25°C 和 5°C）；
  查询 → 2 个 bucket（"20-30"、"0-10"），每个 sample_count=1。

### test_get_monthly_report_with_seeded_data_returns_totals
- 旧版 GetMonthlyReport：2024-06 的 2 条 charging + 1 条 consumption
  （100 km 里程）。返回 total_cost=70，total_kwh=60，total_km=100。

### test_get_annual_report_with_seeded_data_across_months
- 旧版 GetAnnualReport：2024 年 1/2/3 月共 3 条 charging + 1 条 consumption
  （200 km）。返回 month=0（年度哨兵值），total_cost=75，total_kwh=60，
  total_km=200。

## TestErrorPath

### test_get_daily_charging_report_year_too_low_returns_invalid
### test_get_daily_charging_report_month_zero_returns_invalid
### test_get_daily_charging_report_month_thirteen_returns_invalid
### test_get_daily_charging_report_day_zero_returns_invalid
### test_get_daily_charging_report_day_thirtytwo_returns_invalid
### test_get_monthly_charging_report_year_too_low_returns_invalid
### test_get_annual_charging_report_year_too_low_returns_invalid
### test_get_monthly_report_invalid_arguments_returns_invalid
### test_get_annual_report_invalid_year_returns_invalid
- 全部：校验器边界 → INVALID_ARGUMENT。

## TestBoundaries

### test_get_daily_charging_report_feb_30_nonleap_returns_invalid
- 2023-02-30 → INVALID_ARGUMENT（LastDayOfMonth(2023, 2) = 28）。

### test_get_daily_charging_report_feb_29_nonleap_returns_invalid
- 2023-02-29 → INVALID_ARGUMENT。

### test_get_daily_charging_report_feb_29_leap_returns_ok
- 2024-02-29 → OK + 零响应。

### test_get_daily_charging_report_apr_31_returns_invalid
- 2024-04-31 → INVALID_ARGUMENT（四月只有 30 天）。

### test_get_daily_charging_report_jun_31_returns_invalid
- 2024-06-31 → INVALID_ARGUMENT（六月只有 30 天）。

### test_get_daily_charging_report_dec_31_ok
- 2024-12-31 → OK（十二月有 31 天，恰好到上限）。

### test_get_daily_charging_report_year_1900_ok
- year=1900 → OK（最小边界）。

### test_get_daily_charging_report_year_1899_returns_invalid
- year=1899 → INVALID_ARGUMENT（低于 1900）。

### test_get_daily_charging_report_with_vehicle_id_filter
- 随机 UUID 过滤 → count=0，vehicle_id 回显。

## TestConstraints (TZ 感知 + 多 RPC 空响应)

### test_get_monthly_charging_report_with_specific_vehicle
- 在 monthly 上加 vehicle_id 过滤 → count=0。

### test_get_annual_charging_report_with_specific_vehicle
- 在 annual 上加 vehicle_id 过滤 → count=0。

### test_get_cost_by_charger_type_empty_returns_zero_breakdowns
- 随机 vehicle_id → 0 个 breakdown（无数据）。

### test_get_cost_by_source_category_empty_returns_zero_breakdowns
- 随机 vehicle_id → 0 个 breakdown。

### test_get_consumption_efficiency_empty_returns_zero
- 返回响应，0 个 efficiency。

### test_get_range_accuracy_empty_returns_zero
- 返回响应，0 个 accuracy。

### test_get_temperature_consumption_correlation_empty_returns_zero
- 返回响应，0 个 bucket。

### test_vehicle_cost_summary_explicit_1970_01_01_uses_filter
- **Phase Q 回归：** 显式的 1970-01-01T00:00:00Z 时间戳应当作为真实过滤生效，
  而不是被当作 "未设置" 默默丢弃。
- 修复前：`MaybeTimestamp` 用 `seconds() == 0 && nanos() == 0` 启发式判断，
  这与 proto3 默认值（未设置）无法区分。
  用户显式设置 `start_time=1970-01-01` 来 "取所有数据" 时，
  会静默得到一个未过滤的查询。
- 修复后：`MaybeTimestamp` 接收一个 `bool has_value` 参数，
  从父消息的 `has_<field>()` 方法（`req->has_start_time()`）取得。
  现在 `has_value=true + seconds=0` 正确表示 "显式的 1970"，
  过滤会生效。
- Seed：2024-06-15 1 条 charging + 2024-06-16 1 条 consumption，
  100 km 里程增量。
- 断言：`avg_yuan_per_km ≈ 0.35`（= 35 / 100，过滤自 1970 含）。

## TZ-awareness 说明

v1.1.0 RPC 用 `c.StartTime::date = make_date($1, $2, $3)` 和
`EXTRACT(YEAR/MONTH FROM c.StartTime) = $1/2` 操作 TIMESTAMPTZ 列。
Session TZ 来自 PG postmaster 端（**不是** libpq 客户端），
所以测试断言不依赖客户端 TZ。

测试使用 2024-06-15（年中，远离 DST 边界），以避免 TZ 边界的歧义。

## 未覆盖的（按 Chunk 7 计划，超出范围）

- 旧版 `GetMonthlyReport`/`GetAnnualReport`/`GetVehicleCostSummary` 的
  "无数据 → 总计为 0"（生产代码走 EXISTS 预检查触发 INTERNAL；
  不 seed 数据就绕不过去）。
- TZ 边界 case（测试使用稳定的年中日期）。
- 多 RPC 响应除 `vehicle_id` 外的分页/过滤。