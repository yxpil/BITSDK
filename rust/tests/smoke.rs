//! Smoke tests against the fake BIT server (`test/fake_bit_server.js`).
//!
//! Defaults: `BIT_URL=http://127.0.0.1:9803`, `BIT_KEY=bit_test_key_123456`,
//! `BIT_PWD=test-pwd-1`. Start the server with
//! `node test/fake_bit_server.js` from the repo root.
//!
//! The server is shared between concurrent test runs: never assert on global
//! list lengths (tools/audit) — only on objects this test created.

use bit_sdk::{BitClient, BitError, ChatInput, RegisterToolInput};
use serde_json::json;

fn base_url() -> String {
    std::env::var("BIT_URL").unwrap_or_else(|_| "http://127.0.0.1:9803".into())
}

fn key() -> String {
    std::env::var("BIT_KEY").unwrap_or_else(|_| "bit_test_key_123456".into())
}

fn pwd() -> String {
    std::env::var("BIT_PWD").unwrap_or_else(|_| "test-pwd-1".into())
}

fn client() -> BitClient {
    BitClient::new(base_url(), key()).access_password(pwd())
}

fn chat_input(message: &str) -> ChatInput {
    ChatInput {
        message: message.to_string(),
        session_id: None,
        images: vec![],
    }
}

/// Compile-time check that `BitError` implements `std::error::Error`.
fn takes_error(_: &dyn std::error::Error) {}

#[test]
fn health_ok() {
    let h = client().health().unwrap();
    assert_eq!(h["ok"], json!(true));
    assert!(h["version"].as_str().is_some());
}

#[test]
fn register_invoke_remove_roundtrip() {
    let c = client();
    let name = format!(
        "rust_sdk_{}_{:x}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos(),
        std::process::id()
    );
    let tool = c
        .register_tool(RegisterToolInput {
            name: name.clone(),
            description: "rust sdk smoke tool".into(),
            parameters: json!({"type": "object", "properties": {}}),
            url: "http://127.0.0.1:1/hook".into(),
        })
        .unwrap();
    assert!(tool.id.starts_with("t_"));
    assert_eq!(tool.name, name);
    assert_eq!(tool.kind["type"], json!("remote"));

    let out = c
        .invoke_tool(&tool.id, json!({"hello": "世界"}))
        .unwrap();
    assert_eq!(out["via"], json!(name));
    assert_eq!(out["echoed"]["hello"], json!("世界"));

    let removed = c.remove_tool(&tool.id).unwrap();
    assert_eq!(removed, tool.id);
}

#[test]
fn chat_reply_prefix() {
    let r = client().chat(chat_input("rust-smoke hello")).unwrap();
    assert!(r.reply.starts_with("fake reply to:"));
    assert!(r.messages.as_array().map(|m| !m.is_empty()).unwrap_or(false));
}

#[test]
fn debug_state_has_active_model() {
    let s = client().debug_state().unwrap();
    assert!(s["ai"]["active"]["model"].as_str().is_some());
}

#[test]
fn debug_sessions_non_empty() {
    let sessions = client().debug_sessions().unwrap();
    assert!(!sessions.is_empty());
    assert!(!sessions[0].id.is_empty());
}

#[test]
fn debug_session_default_has_messages() {
    let s = client().debug_session("default").unwrap();
    assert_eq!(s["id"], json!("default"));
    assert!(!s["messages"].as_array().unwrap().is_empty());
}

#[test]
fn debug_mcp_lists_tools() {
    let m = client().debug_mcp().unwrap();
    assert!(m["tools"].as_array().is_some());
}

#[test]
fn mcp_initialize_server_info() {
    let resp = client()
        .mcp(json!({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "rust-sdk", "version": "1.0"}
            }
        }))
        .unwrap();
    assert_eq!(resp["result"]["serverInfo"]["name"], json!("fake-bit"));
}

#[test]
fn models_list_two_entries() {
    let m = client().models().unwrap();
    let data = m["data"].as_array().unwrap();
    assert_eq!(data.len(), 2);
    assert_eq!(m["object"], json!("list"));
}

#[test]
fn chat_completions_non_streaming() {
    let resp = client()
        .chat_completions(json!({
            "model": "fake-model",
            "messages": [{"role": "user", "content": "你好"}]
        }))
        .unwrap();
    assert_eq!(
        resp["choices"][0]["message"]["content"],
        json!("你好，世界!")
    );
}

#[test]
fn chat_completions_stream_assembles_multibyte() {
    let mut collected = String::new();
    let mut final_chunk: Option<serde_json::Value> = None;
    let mut content_chunks = 0usize;

    let full = client()
        .chat_completions_stream(
            json!({
                "model": "fake-model",
                "messages": [{"role": "user", "content": "hi"}]
            }),
            |delta, last| {
                if let Some(chunk) = delta {
                    collected.push_str(chunk);
                    content_chunks += 1;
                }
                if let Some(chunk) = last {
                    final_chunk = Some(chunk.clone());
                }
            },
        )
        .unwrap();

    // The fake server sends 你好 / ，世 / 界! — multi-byte chars split across
    // chunks must reassemble losslessly.
    assert_eq!(full, "你好，世界!");
    assert_eq!(collected, "你好，世界!");
    assert!(content_chunks >= 3);

    let f = final_chunk.expect("on_delta(None, Some) must fire with the last parsed chunk");
    assert_eq!(f["choices"][0]["finish_reason"], json!("stop"));
    assert!(f["usage"]["total_tokens"].is_u64());
}

#[test]
fn wrong_key_is_401() {
    let c = BitClient::new(base_url(), "definitely-wrong-key").access_password(pwd());
    let err = c.chat(chat_input("nope")).unwrap_err();
    assert_eq!(err.status, 401);
    assert!(err.message.contains("API Key"), "message: {}", err.message);
    assert!(err.to_string().starts_with("bit: status 401"));
    takes_error(&err);
}

#[test]
fn missing_password_is_401() {
    let c = BitClient::new(base_url(), key());
    let err = c.chat(chat_input("nope")).unwrap_err();
    assert_eq!(err.status, 401);
    assert!(err.message.contains("密码"), "message: {}", err.message);
}

#[test]
fn key_in_query_works() {
    let c = BitClient::new(base_url(), key())
        .access_password(pwd())
        .key_in_query(true);
    let h = c.health().unwrap();
    assert_eq!(h["ok"], json!(true));
    let r = c.chat(chat_input("query-key")).unwrap();
    assert!(r.reply.starts_with("fake reply to:"));
}

// Keep the error type import used even if assertions change.
#[allow(dead_code)]
fn _type_check(_: BitError) {}
