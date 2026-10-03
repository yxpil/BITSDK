# BITSDK 测试说明
- 测试完成：是（2026-10-04）
- 测试日期：2026-10-04
- 测试内容：单元覆盖客户端生命周期/NULL 安全与 `bit_error_message()` 三类错误提取规则；集成覆盖不可信 JSON-RPC / chat-completions 入口在发请求前被拒绝并置 last_error；注入测试通过 stub 捕获真实请求 URL，验证 `tool_id`/`session_id` 中的 `../` 路径穿越被原样拼进路径（客户端不编码，防护在服务端）；本 SDK 为无状态 HTTP 客户端，无钩子/插件注册机制。
- 运行命令：`gcc -Wall -Itests/stubinclude -Iinclude -Isrc -Isrc/cjson tests/test_main.c tests/stub_curl.c src/bitsdk.c src/cjson/cJSON.c -o build/test_unit.exe && ./build/test_unit.exe`
- 测试框架：自建 C assert harness（printf 断言计数）+ 测试专用 stub libcurl
- 模型：豆包（Doubao）生成

---

本目录 `c/tests/` 是**离线单元/集成测试** harness，不需要 libcurl、不需要网络、不需要假服务器。
与既有 `c/test/test_smoke.c`（需启动 `test/fake_bit_server.js` 真发 HTTP）互补。

## 测了什么

### 单元测试
- **生命周期 / NULL 安全**：`bit_client_new(NULL,·)` 返回 NULL；`bit_client_free(NULL)` 不崩溃；`bit_last_error(NULL)` 返回空串。
- **`bit_error_message()` 提取规则**：
  - `{"error":{"message":"boom"}}` → `"boom"`（嵌套 message）
  - `{"error":"plain string err"}` → `"plain string err"`（字符串 error）
  - 无 error 字段 → 回退 `"HTTP 500"`；NULL 响应 → `"HTTP 404"`。

### 集成测试（多个内部函数协作）
- **输入校验（不可信 JSON 入口）**：`bit_mcp(client, "not json")` 与 `bit_chat_completions(client, "{oops")` 在真正发请求前就被拒绝并返回 NULL，同时 `bit_last_error()` 记录原因；`bit_remove_tool(client, NULL)` 拒绝 NULL id。

### 注入测试（路径穿越，必做项）
`tool_id` / `session_id` 被**原样拼接**进 URL 路径（percent-encode 只用于 query 里的 key，不编码路径段）。离线 stub 会把 `CURLOPT_URL` 捕获下来，断言：
- `bit_remove_tool(c, "../../etc/passwd")` → 实际请求路径含 `/api/tools/../../etc/passwd`
- `bit_debug_session(c, "../admin/delete")` → 含 `/api/debug/sessions/../admin/delete`

这如实记录了 SDK **不在客户端做路径段编码**，路径穿越的防护责任在服务端；调用方不应把未过滤的 id 直接传入。

## 钩子测试
本 C SDK 是无状态 HTTP 客户端，无钩子/插件/事件注册机制；流式接口 `bit_chat_completions_stream` 的 `on_delta` 回调由既有在线 smoke 测试覆盖。故本离线 harness 钩子测试数为 0（如实说明）。

## 如何运行（从 `c/` 目录）
```
gcc -Wall -Itests/stubinclude -Iinclude -Isrc -Isrc/cjson \
    tests/test_main.c tests/stub_curl.c src/bitsdk.c src/cjson/cJSON.c \
    -o build/test_unit.exe
./build/test_unit.exe
```
说明：`tests/stubinclude/curl/curl.h` 是一个测试专用的最小 libcurl 替身（把 `<curl/curl.h>` 屏蔽掉），仅用于本地/离线编译，**不参与** `make all` / `make smoke` 的生产构建。

## 预期结果
- 离线 harness：**15 条断言全部通过，0 失败**（单元 13 + 集成 2）。
- 注入测试 2 条（路径穿越）；输入校验 2 条；钩子测试 0 条。
- 既有在线 smoke（`make test`）需先 `node test/fake_bit_server.js`，本机未运行。
