# test_source_category.md

## Overview
- **Service:** SourceCategoryService
- **RPCs:** CreateSourceCategory, SearchSourceCategory
- **Total tests:** 7 (2 TestHappyPath + 1 TestErrorPath + 4 TestBoundaries parametrize)
- **Purpose:** Mirror Weather test pattern — same Create + prefix-search shape.

## TestHappyPath

### test_create_source_category_returns_id_and_name
- **RPC:** SourceCategoryService.CreateSourceCategory
- **Purpose:** Verify Create returns UUID + echoed name.
- **Action:** CreateSourceCategory with valid name.
- **Expected:** Non-empty `id`, `name == req.name`.

### test_search_source_category_finds_created
- **RPC:** SourceCategoryService.SearchSourceCategory
- **Purpose:** Search returns OK + iterable matches after creating a row.
- **Note:** Search verification happens inside the `with TrackedInsert` block (registers the match's id for cleanup); outer search just confirms response shape, not the specific row (which was deleted by cleanup).
- **Pattern:** Same as Weather `test_search_weather_finds_created_weather` — see that doc for the in-vs-out `with` block rationale.

## TestErrorPath

### test_create_source_category_duplicate_name_returns_already_exists
- **UNIQUE** on `source_category.Name` → `ALREADY_EXISTS` (per `error.cc`).

## TestBoundaries

### test_create_source_category_name_length
- **Parametrize IDs:** `[1]`, `[35]`, `[36]`, `[37]`
- **VARCHAR(36):** 1/35/36 = OK; 37 = INVALID_ARGUMENT.
- **Same boundary pattern as `test_create_weather_name_length`** — see Weather doc.