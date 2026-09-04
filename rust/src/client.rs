use std::io::{BufRead, BufReader};
use std::time::Duration;

use serde::de::DeserializeOwned;

use crate::error::{parse_error, BitError, Result};
use crate::types::{
    AuditResponse, ChatInput, ChatResult, InvokeResponse, RegisterToolInput, RemoveResponse,
    SessionsResponse, ToolDef, ToolResponse, ToolsResponse,
};

const DEFAULT_TIMEOUT: Duration = Duration::from_secs(30);

/// Client for the BIT desktop agent HTTP API (BIT v0.5.2, `docs/API.md`).
///
/// Construct with [`BitClient::new`] and the optional builder methods
/// ([`access_password`](BitClient::access_password),
/// [`timeout`](BitClient::timeout),
/// [`key_in_query`](BitClient::key_in_query)).
#[derive(Debug, Clone)]
pub struct BitClient {
    base_url: String,
    client_key: String,
    access_password: Option<String>,
    timeout: Duration,
    key_in_query: bool,
}

impl BitClient {
    /// Create a client: default 30s timeout, no access password, Client Key
    /// sent as `Authorization: Bearer`.
    pub fn new(base_url: impl Into<String>, client_key: impl Into<String>) -> Self {
        Self {
            base_url: base_url.into(),
            client_key: client_key.into(),
            access_password: None,
            timeout: DEFAULT_TIMEOUT,
            key_in_query: false,
        }
    }

    /// Set the access password sent as `X-Access-Password`. Required on
    /// `/api/*`; `/v1/*` and `/mcp` are password-exempt (the header is
    /// ignored there, so it is harmless to always send it).
    pub fn access_password(mut self, password: impl Into<String>) -> Self {
        self.access_password = Some(password.into());
        self
    }

    /// Override the request timeout (default 30s).
    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    /// Send the Client Key as `?key=<key>` query parameter instead of the
    /// `Authorization` header (for clients that cannot set headers).
    pub fn key_in_query(mut self, key_in_query: bool) -> Self {
        self.key_in_query = key_in_query;
        self
    }

    // ---- transport --------------------------------------------------------

    fn agent(&self) -> ureq::Agent {
        ureq::AgentBuilder::new().timeout(self.timeout).build()
    }

    fn url(&self, path: &str) -> String {
        let mut url = format!("{}{}", self.base_url.trim_end_matches('/'), path);
        if self.key_in_query {
            url.push(if url.contains('?') { '&' } else { '?' });
            url.push_str("key=");
            url.push_str(&percent_encode(&self.client_key));
        }
        url
    }

    fn request(
        &self,
        method: &str,
        path: &str,
        body: Option<&serde_json::Value>,
    ) -> Result<ureq::Response> {
        let req = self.agent().request(method, &self.url(path));
        let req = if self.key_in_query {
            req
        } else {
            req.set("Authorization", &format!("Bearer {}", self.client_key))
        };
        let req = match &self.access_password {
            Some(pw) => req.set("X-Access-Password", pw),
            None => req,
        };
        let sent = match body {
            Some(v) => req.send_json(v),
            None => req.call(),
        };
        match sent {
            Ok(resp) => Ok(resp),
            Err(ureq::Error::Status(status, resp)) => {
                let text = resp.into_string().unwrap_or_default();
                Err(parse_error(status, &text))
            }
            Err(ureq::Error::Transport(t)) => Err(BitError {
                status: 0,
                message: t.to_string(),
                raw: None,
            }),
        }
    }

    /// Decode a success body: read text → parse JSON → fit the target type.
    /// `raw` is populated when the body is valid JSON of an unexpected shape.
    fn decode<T: DeserializeOwned>(resp: ureq::Response) -> Result<T> {
        let status = resp.status();
        let text = resp.into_string().map_err(|e| BitError {
            status,
            message: format!("failed to read response body: {e}"),
            raw: None,
        })?;
        let v: serde_json::Value = serde_json::from_str(&text).map_err(|e| BitError {
            status,
            message: format!("invalid JSON response: {e}"),
            raw: None,
        })?;
        serde_json::from_value(v.clone()).map_err(|e| BitError {
            status,
            message: format!("unexpected response shape: {e}"),
            raw: Some(v),
        })
    }

    fn encode<T: serde::Serialize>(value: T) -> Result<serde_json::Value> {
        serde_json::to_value(value).map_err(|e| BitError {
            status: 0,
            message: format!("failed to serialize request body: {e}"),
            raw: None,
        })
    }

    // ---- /api/* -----------------------------------------------------------

    /// `GET /api/health` — liveness probe (no auth).
    pub fn health(&self) -> Result<serde_json::Value> {
        Self::decode(self.request("GET", "/api/health", None)?)
    }

    /// `GET /api/tools` — list registered tools.
    pub fn list_tools(&self) -> Result<Vec<ToolDef>> {
        let resp: ToolsResponse = Self::decode(self.request("GET", "/api/tools", None)?)?;
        Ok(resp.tools)
    }

    /// `POST /api/tools` — register a remote tool, returns the created
    /// [`ToolDef`] (`201`). `409` when the name is already registered.
    pub fn register_tool(&self, input: RegisterToolInput) -> Result<ToolDef> {
        let body = Self::encode(input)?;
        let resp: ToolResponse = Self::decode(self.request("POST", "/api/tools", Some(&body))?)?;
        Ok(resp.tool)
    }

    /// `DELETE /api/tools/{id}` — returns the removed id.
    pub fn remove_tool(&self, id: &str) -> Result<String> {
        let resp: RemoveResponse =
            Self::decode(self.request("DELETE", &format!("/api/tools/{id}"), None)?)?;
        Ok(resp.removed)
    }

    /// `POST /api/tools/{id}/invoke` — returns the tool's own return value.
    pub fn invoke_tool(&self, id: &str, params: serde_json::Value) -> Result<serde_json::Value> {
        let body = serde_json::json!({ "params": params });
        let resp: InvokeResponse = Self::decode(
            self.request("POST", &format!("/api/tools/{id}/invoke"), Some(&body))?,
        )?;
        Ok(resp.result)
    }

    /// `POST /api/chat` — run one full agent turn.
    pub fn chat(&self, input: ChatInput) -> Result<ChatResult> {
        let body = Self::encode(input)?;
        Self::decode(self.request("POST", "/api/chat", Some(&body))?)
    }

    /// `GET /api/audit` — audit trail entries (newest first on the server).
    pub fn audit(&self) -> Result<Vec<crate::types::AuditEntry>> {
        let resp: AuditResponse = Self::decode(self.request("GET", "/api/audit", None)?)?;
        Ok(resp.entries)
    }

    /// `GET /api/debug/state` — read-only state snapshot for debugging.
    pub fn debug_state(&self) -> Result<serde_json::Value> {
        Self::decode(self.request("GET", "/api/debug/state", None)?)
    }

    /// `GET /api/debug/sessions` — session summaries.
    pub fn debug_sessions(&self) -> Result<Vec<crate::types::SessionSummary>> {
        let resp: SessionsResponse = Self::decode(self.request("GET", "/api/debug/sessions", None)?)?;
        Ok(resp.sessions)
    }

    /// `GET /api/debug/sessions/{id}` — full session with messages.
    pub fn debug_session(&self, id: &str) -> Result<serde_json::Value> {
        Self::decode(self.request("GET", &format!("/api/debug/sessions/{id}"), None)?)
    }

    /// `GET /api/debug/mcp` — MCP tools aggregated from configured servers.
    pub fn debug_mcp(&self) -> Result<serde_json::Value> {
        Self::decode(self.request("GET", "/api/debug/mcp", None)?)
    }

    // ---- /mcp and /v1/* ----------------------------------------------------

    /// `POST /mcp` — raw JSON-RPC 2.0 request, raw JSON-RPC response
    /// (BIT acts as MCP server; password-exempt).
    pub fn mcp(&self, payload: serde_json::Value) -> Result<serde_json::Value> {
        Self::decode(self.request("POST", "/mcp", Some(&payload))?)
    }

    /// `GET /v1/models` — OpenAI-compatible model list (password-exempt).
    pub fn models(&self) -> Result<serde_json::Value> {
        Self::decode(self.request("GET", "/v1/models", None)?)
    }

    /// `POST /v1/chat/completions` — non-streaming OpenAI-compatible call;
    /// pass a standard request body, get the standard response object.
    pub fn chat_completions(&self, body: serde_json::Value) -> Result<serde_json::Value> {
        Self::decode(self.request("POST", "/v1/chat/completions", Some(&body))?)
    }

    /// `POST /v1/chat/completions` with `stream: true` — SSE streaming.
    ///
    /// `on_delta(Some(chunk), None)` is invoked per content chunk;
    /// `on_delta(None, Some(final))` once with the last parsed chunk before
    /// `data: [DONE]` (may carry `usage`). Returns the assembled full text.
    ///
    /// The decoder buffers raw bytes and splits only on `\n`
    /// (`BufRead::read_until`), so multi-byte UTF-8 characters split across
    /// TCP chunks are handled safely.
    pub fn chat_completions_stream(
        &self,
        mut body: serde_json::Value,
        mut on_delta: impl FnMut(Option<&str>, Option<&serde_json::Value>),
    ) -> Result<String> {
        if let Some(obj) = body.as_object_mut() {
            obj.insert("stream".to_string(), serde_json::Value::Bool(true));
        }
        let resp = self.request("POST", "/v1/chat/completions", Some(&body))?;
        let mut reader = BufReader::new(resp.into_reader());

        let mut full = String::new();
        let mut line: Vec<u8> = Vec::new();
        let mut last_chunk: Option<serde_json::Value> = None;

        loop {
            line.clear();
            let n = reader.read_until(b'\n', &mut line).map_err(|e| BitError {
                status: 0,
                message: format!("stream read failed: {e}"),
                raw: None,
            })?;
            if n == 0 {
                break; // EOF without [DONE]: return what we have
            }
            while matches!(line.last(), Some(b'\n') | Some(b'\r')) {
                line.pop();
            }
            if line.is_empty() {
                continue; // blank SSE separator line
            }
            let line = String::from_utf8(line.clone()).map_err(|e| BitError {
                status: 0,
                message: format!("stream line is not valid UTF-8: {e}"),
                raw: None,
            })?;
            let Some(payload) = line.strip_prefix("data:") else {
                continue; // ignore other SSE fields
            };
            let payload = payload.trim();
            if payload == "[DONE]" {
                break;
            }
            let Ok(chunk) = serde_json::from_str::<serde_json::Value>(payload) else {
                continue; // tolerate malformed keep-alive lines
            };
            if let Some(text) = chunk
                .pointer("/choices/0/delta/content")
                .and_then(|v| v.as_str())
            {
                full.push_str(text);
                on_delta(Some(text), None);
            }
            last_chunk = Some(chunk);
        }

        if let Some(chunk) = &last_chunk {
            on_delta(None, Some(chunk));
        }
        Ok(full)
    }
}

/// Percent-encode for the `?key=` query parameter (RFC 3986 unreserved set).
fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for b in s.bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(b as char)
            }
            _ => out.push_str(&format!("%{b:02X}")),
        }
    }
    out
}
