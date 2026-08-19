# test_smoke.md

## 概述
- **服务：** WeatherService（健全性检查）
- **测试总数：** 1
- **目的：** 端到端健全性检查，验证测试栈是否正常工作
  (conftest fixtures + auth + nginx:80 → evgrpc:50051 → Postgres)。
  这**不是**覆盖率测试 —— 而是一个守护测试，
  防止整个测试栈被破坏
  （docker-compose 停掉、OIDC IdP 不可达、schema migration 把表删了）。

## TestHappyPath

### test_search_weather_with_valid_bearer_succeeds
- **RPC：** WeatherService.SearchWeather
- **目的：** 用从 auth-test.mksword.com 拿到的真实 Bearer token，
  端到端验证完整的 auth + channel + RPC 栈能正常工作。
- **前置：** 无（依赖 session fixtures）。
- **操作：** 使用 `channel` fixture 注入的 bearer token，
  调用 `SearchWeather(prefix="", limit=1)`。
- **预期：** 响应是 `SearchWeatherResponse`（没有 UNAUTHENTICATED、
  没有 gRPC 错误）。空列表可接受。
- **清理：** 无（仅探测，不写 DB）。
- **关联：** `tests/python/` 里所有其他测试都依赖这个栈健康。
  如果这个测试失败，其他测试也会失败。