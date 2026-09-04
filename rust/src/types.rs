use serde::{Deserialize, Serialize};

fn default_true() -> bool {
    true
}

/// A tool registered in BIT.
///
/// Unknown fields are ignored; known fields are optional where the contract
/// allows (`docs/API.md`).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ToolDef {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub parameters: serde_json::Value,
    /// `kind.type` ∈ `builtin | remote | script | interpreter | mcp`.
    #[serde(default)]
    pub kind: serde_json::Value,
    #[serde(default)]
    pub created_by: String,
    #[serde(default)]
    pub created_at: String,
    #[serde(default = "default_true")]
    pub enabled: bool,
}

/// Input for [`crate::BitClient::register_tool`] (POST /api/tools).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisterToolInput {
    pub name: String,
    pub description: String,
    /// JSON schema of the tool parameters.
    pub parameters: serde_json::Value,
    /// Callback URL BIT will POST tool calls to.
    pub url: String,
}

/// Input for [`crate::BitClient::chat`] (POST /api/chat).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ChatInput {
    pub message: String,
    /// Optional; creates the session if missing; omit = current active session.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    /// Optional data-URL images, only for vision models.
    #[serde(default)]
    pub images: Vec<String>,
}

/// Response of [`crate::BitClient::chat`].
#[derive(Debug, Clone, Deserialize)]
pub struct ChatResult {
    /// The last assistant message.
    pub reply: String,
    /// Full session messages (array of `{role, content, ...}` objects).
    #[serde(default)]
    pub messages: serde_json::Value,
}

/// One audit entry (GET /api/audit).
#[derive(Debug, Clone, Deserialize)]
pub struct AuditEntry {
    #[serde(default)]
    pub ts: String,
    #[serde(default)]
    pub actor: String,
    #[serde(default)]
    pub action: String,
    #[serde(default)]
    pub target: String,
    #[serde(default)]
    pub detail: serde_json::Value,
    #[serde(default = "default_true")]
    pub ok: bool,
}

/// Session summary (GET /api/debug/sessions).
#[derive(Debug, Clone, Deserialize)]
pub struct SessionSummary {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub messages: u64,
    #[serde(default)]
    pub ts: String,
}

// ---- private response wrappers -------------------------------------------

#[derive(Deserialize)]
pub(crate) struct ToolsResponse {
    #[serde(default)]
    pub tools: Vec<ToolDef>,
}

#[derive(Deserialize)]
pub(crate) struct ToolResponse {
    pub tool: ToolDef,
}

#[derive(Deserialize)]
pub(crate) struct RemoveResponse {
    pub removed: String,
}

#[derive(Deserialize)]
pub(crate) struct InvokeResponse {
    #[serde(default)]
    pub result: serde_json::Value,
}

#[derive(Deserialize)]
pub(crate) struct AuditResponse {
    #[serde(default)]
    pub entries: Vec<AuditEntry>,
}

#[derive(Deserialize)]
pub(crate) struct SessionsResponse {
    #[serde(default)]
    pub sessions: Vec<SessionSummary>,
}
