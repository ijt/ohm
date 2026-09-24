//! ohm-browser-passkey: one WebAuthn ceremony with a phone over the hybrid
//! transport (the QR code + Bluetooth flow Chrome calls "use a phone or
//! tablet"), for Ohm's navigator.credentials shim.
//!
//! Ohm starts one process per ceremony and writes a single JSON line to
//! stdin:
//!
//!   {"type": "create" | "get",
//!    "origin": "https://example.com",       // from Chromium, not the page
//!    "topOrigin": "https://top.example",    // only for cross-origin iframes
//!    "options": { ...PublicKeyCredential{Creation,Request}OptionsJSON... }}
//!
//! and reads JSON lines back from stdout:
//!
//!   {"qr": {"size": 33, "modules": "0101..."}}   // row-major, 1 = dark
//!   {"status": "proximity" | "connecting" | "authenticating" | "connected" | "touch"}
//!   {"result": { ...RegistrationResponseJSON / AuthenticationResponseJSON... }}
//!   {"error": {"name": "NotAllowedError", "message": "..."}}
//!
//! `result` or `error` is always the last line. Cancelling is killing the
//! process. rp.id is validated against `origin` here (public suffix list),
//! and clientDataJSON is built here from `origin`, so a page can only ever
//! get credentials for itself.

use std::io::{self, BufRead, Write};

use libwebauthn::ops::webauthn::{
    DatFilePublicSuffixList, GetAssertionPrepareError, GetAssertionRequest, JsonFormat,
    MakeCredentialPrepareError, MakeCredentialRequest, OriginValidation, RelatedOrigins,
    RequestOrigin, RequestSettings, WebAuthnIDLResponse as _,
};
use libwebauthn::transport::cable::channel::{CableUpdate, CableUxUpdate};
use libwebauthn::transport::cable::is_available;
use libwebauthn::transport::cable::qr_code_device::{
    CableQrCodeDevice, CableTransports, QrCodeOperationHint,
};
use libwebauthn::transport::{Channel as _, ChannelSettings, Device as _};
use libwebauthn::webauthn::{CtapError, PlatformError, WebAuthn as _, WebAuthnError};
use libwebauthn::UvUpdate;
use serde_json::{json, Value};

/// A DOMException for the page: `name` is what sites switch on.
struct DomError {
    name: &'static str,
    message: String,
}

fn dom(name: &'static str, message: impl Into<String>) -> DomError {
    DomError { name, message: message.into() }
}

fn emit(line: Value) {
    let mut out = io::stdout().lock();
    let _ = writeln!(out, "{line}");
    let _ = out.flush();
}

// WebAuthn's error model deliberately collapses almost everything the user
// or authenticator can do into NotAllowedError, so a page can't tell
// "cancelled" from "no such credential" (a privacy property).
fn from_ceremony<E: std::fmt::Display + std::fmt::Debug>(err: WebAuthnError<E>) -> DomError {
    match err {
        WebAuthnError::Ctap(CtapError::CredentialExcluded) => {
            dom("InvalidStateError", "A passkey for this account already exists on that device.")
        }
        WebAuthnError::Ctap(CtapError::UnsupportedAlgorithm) => {
            dom("NotSupportedError", "The phone doesn't support any requested algorithm.")
        }
        WebAuthnError::Platform(PlatformError::NotSupported) => {
            dom("NotSupportedError", "The phone doesn't support this request.")
        }
        other => dom("NotAllowedError", other.to_string()),
    }
}

fn from_prepare_mc(err: MakeCredentialPrepareError) -> DomError {
    match err {
        MakeCredentialPrepareError::EncodingError(e) => dom("TypeError", e.to_string()),
        other => dom("SecurityError", other.to_string()),
    }
}

fn from_prepare_ga(err: GetAssertionPrepareError) -> DomError {
    match err {
        GetAssertionPrepareError::EncodingError(e) => dom("TypeError", e.to_string()),
        GetAssertionPrepareError::NotSupported(m) => dom("NotSupportedError", m),
        e @ GetAssertionPrepareError::UnexpectedLengthError(..) => dom("TypeError", e.to_string()),
        other => dom("SecurityError", other.to_string()),
    }
}

fn emit_qr(payload: &str) -> Result<(), DomError> {
    let code = qrcode::QrCode::new(payload.as_bytes())
        .map_err(|e| dom("UnknownError", format!("QR encoding failed: {e}")))?;
    let modules: String = code
        .to_colors()
        .iter()
        .map(|c| if *c == qrcode::Color::Dark { '1' } else { '0' })
        .collect();
    emit(json!({"qr": {"size": code.width(), "modules": modules}}));
    Ok(())
}

async fn forward_updates(mut rx: tokio::sync::broadcast::Receiver<CableUxUpdate>) {
    while let Ok(update) = rx.recv().await {
        let status = match update {
            CableUxUpdate::CableUpdate(CableUpdate::ProximityCheck) => "proximity",
            CableUxUpdate::CableUpdate(CableUpdate::Connecting) => "connecting",
            CableUxUpdate::CableUpdate(CableUpdate::Authenticating) => "authenticating",
            CableUxUpdate::CableUpdate(CableUpdate::Connected) => "connected",
            CableUxUpdate::UvUpdate(UvUpdate::PresenceRequired) => "touch",
            _ => continue,
        };
        emit(json!({"status": status}));
    }
}

/// Retries only what the phone reports as "try that again" (a failed Face
/// ID, a user-action timeout), and not forever.
macro_rules! with_retries {
    ($call:expr) => {{
        let mut attempts = 0;
        loop {
            match $call.await {
                Err(WebAuthnError::Ctap(e)) if e.is_retryable_user_error() && attempts < 3 => {
                    attempts += 1;
                }
                other => break other,
            }
        }
    }};
}

async fn run(req: Value) -> Result<Value, DomError> {
    let kind = req["type"].as_str().unwrap_or_default().to_owned();
    let hint = match kind.as_str() {
        "create" => QrCodeOperationHint::MakeCredential,
        "get" => QrCodeOperationHint::GetAssertionRequest,
        _ => return Err(dom("TypeError", "type must be \"create\" or \"get\"")),
    };
    let origin_str = req["origin"].as_str().ok_or_else(|| dom("SecurityError", "no origin"))?;
    let origin = origin_str
        .parse()
        .map_err(|e| dom("SecurityError", format!("bad origin {origin_str}: {e:?}")))?;
    let request_origin = match req["topOrigin"].as_str() {
        Some(top) => RequestOrigin::new_cross_origin(
            origin,
            top.parse().map_err(|e| dom("SecurityError", format!("bad top origin {top}: {e:?}")))?,
        ),
        None => RequestOrigin::new(origin),
    };
    // appid / appidExclude only let a site reach credentials from the U2F
    // era (security keys registered under an AppID URL). A phone passkey is
    // never one, so over hybrid they can't change the outcome -- but
    // libwebauthn validates appid more strictly than Chrome (it won't take
    // same-site https://api.x.com for https://x.com), which failed x.com's
    // passkey sign-in outright. Drop them and report appid as unused; rp.id
    // is still checked against the origin in full.
    let mut options = req["options"].clone();
    let had_appid = match options.get_mut("extensions").and_then(Value::as_object_mut) {
        Some(ext) => {
            ext.remove("appidExclude");
            ext.remove("appid").is_some()
        }
        None => false,
    };
    let options = options.to_string();
    tracing::info!(%kind, origin = origin_str, %options, "ohm-browser-passkey request");

    let psl = DatFilePublicSuffixList::from_system_file().map_err(|e| {
        dom("NotSupportedError", format!("public suffix list unavailable (install publicsuffix-list): {e}"))
    })?;
    let settings = RequestSettings {
        origin: OriginValidation::Validate {
            public_suffix_list: &psl,
            related_origins: RelatedOrigins::Disabled,
        },
    };

    // Validate before showing a QR code: a request the page isn't allowed
    // to make should fail at once, not after the user reaches for a phone.
    enum Prepared {
        Create(MakeCredentialRequest),
        Get(GetAssertionRequest),
    }
    let prepared = if kind == "create" {
        Prepared::Create(
            MakeCredentialRequest::prepare(&request_origin, &options, &settings)
                .await
                .map_err(from_prepare_mc)?,
        )
    } else {
        Prepared::Get(
            GetAssertionRequest::prepare(&request_origin, &options, &settings)
                .await
                .map_err(from_prepare_ga)?,
        )
    };

    if !is_available().await {
        return Err(dom("NotAllowedError", "Bluetooth is off or missing; a phone needs it to prove it's nearby."));
    }

    let mut device = CableQrCodeDevice::new_transient(hint, CableTransports::CloudAssistedOrLocal)
        .map_err(|e| dom("NotAllowedError", format!("{e:?}")))?;
    emit_qr(&device.qr_code.to_string())?;

    let mut channel = device
        .channel(ChannelSettings::default())
        .await
        .map_err(|e| dom("NotAllowedError", format!("{e:?}")))?;
    tokio::spawn(forward_updates(channel.get_ux_update_receiver()));

    let json = match &prepared {
        Prepared::Create(request) => {
            let response = with_retries!(channel.webauthn_make_credential(request)).map_err(from_ceremony)?;
            response.to_json_string(request, JsonFormat::Minified)
        }
        Prepared::Get(request) => {
            let response = with_retries!(channel.webauthn_get_assertion(request)).map_err(from_ceremony)?;
            for (i, a) in response.assertions.iter().enumerate() {
                tracing::info!(
                    assertion = i,
                    credential_id_len = a.credential_id.as_ref().map(|c| c.id.len()),
                    user_handle_len = a.user.as_ref().map(|u| u.id.len()),
                    flags = ?a.authenticator_data.flags,
                    "phone returned an assertion"
                );
            }
            // The phone does its own account picking, so hybrid returns one.
            let assertion = response
                .assertions
                .first()
                .ok_or_else(|| dom("NotAllowedError", "The phone returned no passkey."))?;
            assertion.to_json_string(request, JsonFormat::Minified)
        }
    }
    .map_err(|e| dom("UnknownError", format!("{e:?}")))?;
    let mut result: Value = serde_json::from_str(&json).map_err(|e| dom("UnknownError", e.to_string()))?;
    if had_appid {
        if !result["clientExtensionResults"].is_object() {
            result["clientExtensionResults"] = json!({});
        }
        result["clientExtensionResults"]["appid"] = json!(false);
    }
    Ok(result)
}

#[tokio::main]
async fn main() {
    // stderr reaches Ohm's journal (journalctl --user -u ohm-browser.service).
    // OHM_PASSKEY_LOG takes an env-filter, e.g. "libwebauthn=debug".
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_env("OHM_PASSKEY_LOG")
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn")),
        )
        .with_writer(io::stderr)
        .with_ansi(false)
        .without_time()
        .init();

    let mut line = String::new();
    if io::stdin().lock().read_line(&mut line).is_err() {
        std::process::exit(2);
    }
    let outcome = match serde_json::from_str::<Value>(&line) {
        Ok(req) => run(req).await,
        Err(e) => Err(dom("TypeError", format!("bad request: {e}"))),
    };
    match outcome {
        Ok(result) => emit(json!({"result": result})),
        Err(e) => {
            tracing::warn!(name = e.name, message = %e.message, "ohm-browser-passkey failed");
            emit(json!({"error": {"name": e.name, "message": e.message}}))
        }
    }
    // Background tunnel tasks may still be winding down; they have nothing
    // left to say.
    std::process::exit(0);
}
