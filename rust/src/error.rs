use std::fmt;

/// Single error type for every SDK failure.
///
/// Carries the HTTP status plus the message parsed from the BIT `error`
/// field. Transport-level failures (DNS, connect, read, decode) use
/// `status = 0`.
#[derive(Debug, Clone)]
pub struct BitError {
    /// HTTP status code, or `0` for transport-level failures.
    pub status: u16,
    /// Human-readable message from the `error` field (or the transport text).
    pub message: String,
    /// Raw JSON body when the server sent parseable JSON.
    pub raw: Option<serde_json::Value>,
}

impl fmt::Display for BitError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "bit: status {}: {}", self.status, self.message)
    }
}

impl std::error::Error for BitError {}

/// Convenience alias used throughout the SDK.
pub type Result<T> = std::result::Result<T, BitError>;

/// Parse a BIT error body into a [`BitError`].
///
/// Handles both `{"error": "..."}` and the OpenAI-style
/// `{"error": {"message": ..., ...}}` shape; falls back to the raw text.
pub(crate) fn parse_error(status: u16, body: &str) -> BitError {
    let text = body.trim();
    match serde_json::from_str::<serde_json::Value>(text) {
        Ok(v) => {
            let message = match v.get("error") {
                Some(serde_json::Value::String(s)) => s.clone(),
                Some(e @ serde_json::Value::Object(_)) => e
                    .get("message")
                    .and_then(|m| m.as_str())
                    .map(str::to_string)
                    .unwrap_or_else(|| text.to_string()),
                _ => text.to_string(),
            };
            BitError {
                status,
                message,
                raw: Some(v),
            }
        }
        Err(_) => BitError {
            status,
            message: text.to_string(),
            raw: None,
        },
    }
}
