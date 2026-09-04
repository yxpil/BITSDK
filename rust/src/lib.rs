//! # bit-sdk
//!
//! Rust SDK for the BIT desktop agent HTTP API (BIT v0.5.2).
//!
//! The binding contract — endpoints, auth rules, error body shapes, SSE
//! streaming format — lives in [`../docs/API.md`](../docs/API.md) and this
//! crate implements exactly that.
//!
//! - Sync HTTP only (`ureq`), no async runtime required.
//! - Auth: Client Key via `Authorization: Bearer` (or `?key=` with
//!   [`BitClient::key_in_query`]) plus `X-Access-Password` on `/api/*`
//!   (`/v1/*` and `/mcp` are password-exempt).
//! - Every failure surfaces as [`BitError`] carrying the HTTP status and the
//!   message parsed from the `error` field.

mod client;
mod error;
mod types;

pub use client::BitClient;
pub use error::{BitError, Result};
pub use types::{AuditEntry, ChatInput, ChatResult, RegisterToolInput, SessionSummary, ToolDef};
