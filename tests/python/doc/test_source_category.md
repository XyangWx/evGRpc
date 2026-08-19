# test_source_category.md

## 概述
- **服务：** SourceCategoryService
- **RPC：** CreateSourceCategory, SearchSourceCategory
- **测试总数：** 7（2 个 TestHappyPath + 1 个 TestErrorPath + 4 个 TestBoundaries 参数化）
- **目的：** 复刻 Weather 测试的模式 —— 同样的 Create + prefix-search 结构。

## TestHappyPath

### test_create_source_category_returns_id_and_name
- **RPC：** SourceCategoryService.CreateSourceCategory
- **目的：** 验证 Create 返回 UUID 和回显的 name。
- **操作：** 用合法 name 调用 CreateSourceCategory。
- **预期：** `id` 非空，`name == req.name`。

### test_search_source_category_finds_created
- **RPC：** SourceCategoryService.SearchSourceCategory
- **目的：** 创建一行后，Search 返回 OK + 可迭代的匹配项。
- **注意：** Search 验证发生在 `with TrackedInsert` 块内部
  （注册匹配项的 id 用于清理）；外层 search 只是确认响应结构，
  不验证具体那一行（清理时已被删除）。
- **模式：** 与 Weather 的 `test_search_weather_finds_created_weather` 相同
  —— 关于 in-block vs out-of-block `with` 的理由请参阅那个文档。

## TestErrorPath

### test_create_source_category_duplicate_name_returns_already_exists
- `source_category.Name` 上的 UNIQUE 约束 → `ALREADY_EXISTS`
  （依据 `error.cc`）。

## TestBoundaries

### test_create_source_category_name_length
- **参数化 ID：** `[1]`, `[35]`, `[36]`, `[37]`
- **VARCHAR(36)：** 1/35/36 = OK；37 = INVALID_ARGUMENT。
- **边界模式与 `test_create_weather_name_length` 一致** —— 见 Weather 文档。