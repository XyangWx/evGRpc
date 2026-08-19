# test_auth_enforcement.md

## 概述
- **服务：** WeatherService（仅作为探测目标）
- **测试总数：** 3
- **目的：** 验证 evgrpc 的 JWT validator 能拒绝非法 token。

## TestHappyPath (验证路径)

### test_no_token_returns_unauthenticated
- **目的：** 没有 `Authorization` header → UNAUTHENTICATED。
- **前置：** 裸 insecure_channel（不带 Bearer interceptor）。
- **操作：** 调用 SearchWeather()，不带任何 metadata。
- **预期：** `grpc.StatusCode.UNAUTHENTICATED`。

### test_malformed_token_returns_unauthenticated
- **目的：** 格式错误的 Bearer token（不是合法的 JWT）→ UNAUTHENTICATED。
- **操作：** metadata=("authorization", "Bearer not.a.real.jwt")。
- **预期：** UNAUTHENTICATED。

### test_forged_token_returns_unauthenticated
- **目的：** 用一次性 RSA 密钥伪造的 JWT（iss/aud 是真的，但 kid 是假的）
  → UNAUTHENTICATED。
- **前置：** 进程内生成 RSA-2048 密钥；用与真实 IdP 相同的 iss/aud claims
  签 JWT，但 signing key 用一次性的，kid 设为 `"forged-key"`。
- **操作：** 带 `Authorization: Bearer <forged>` 调用 SearchWeather。
- **预期：** UNAUTHENTICATED（服务端 JWT validator 在 JWKS 里查 `kid`，
  找不到 "forged-key"，签名校验失败）。

## 使用的 Claims（对照真实颁发的 token 验证过）

- `iss = "https://auth-test.mksword.com/"`
- `aud = "https://www.mksword.com/grpc/ev"`
- `kid = "19C1B8A78C7648CA5DC4EAEFDEE51986991DBC51"`（真实 kid；
  在 forged 测试中不使用 —— 那里用的是 "forged-key"）

## 未覆盖的（按 spec §10 范围之外）

- RBAC / 按 RPC 的 scope 强制（等待服务端加入 scope 检查）。
- Expired-token 测试（需要 IdP 管理员操作 —— 公开的
  client_credentials 流程发不出短有效期的 token）。