use axum::{
    body::Bytes,
    extract::State,
    http::{HeaderMap, StatusCode},
    response::IntoResponse,
    routing::{get, post},
    Router,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use rand::{rngs::OsRng, RngCore};
use rusqlite::{params, Connection, OptionalExtension, Transaction};
use std::{
    env,
    net::SocketAddr,
    path::Path,
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};

const INVITATION_TTL_MS: i64 = 10 * 60 * 1000;
const SESSION_TTL_MS: i64 = 15 * 60 * 1000;
const SIGNATURE_SKEW_SECONDS: i64 = 5 * 60;
const MAX_BODY_BYTES: usize = 256 * 1024;
const MAX_SIGNAL_BYTES: usize = 64 * 1024;
const MAX_DRAIN_SIGNALS: i64 = 256;

#[derive(Clone)]
struct AppState {
    database: Arc<Mutex<Connection>>,
}

#[derive(Debug)]
struct ApiError {
    status: StatusCode,
    message: String,
}

impl ApiError {
    fn bad(message: impl Into<String>) -> Self {
        Self { status: StatusCode::BAD_REQUEST, message: message.into() }
    }
    fn unauthorized(message: impl Into<String>) -> Self {
        Self { status: StatusCode::UNAUTHORIZED, message: message.into() }
    }
    fn forbidden(message: impl Into<String>) -> Self {
        Self { status: StatusCode::FORBIDDEN, message: message.into() }
    }
    fn conflict(message: impl Into<String>) -> Self {
        Self { status: StatusCode::CONFLICT, message: message.into() }
    }
    fn internal(message: impl Into<String>) -> Self {
        Self { status: StatusCode::INTERNAL_SERVER_ERROR, message: message.into() }
    }
}

impl IntoResponse for ApiError {
    fn into_response(self) -> axum::response::Response {
        (self.status, format!("{}\n", self.message)).into_response()
    }
}

type ApiResult = Result<String, ApiError>;

#[derive(Clone)]
struct DeviceAuth {
    device_id: String,
    public_key: String,
    fingerprint: String,
}

#[derive(Clone)]
struct Member {
    device_id: String,
    fingerprint: String,
    role: String,
}

#[derive(Clone)]
struct Enrollment {
    room_id: String,
    authorization_digest: String,
    session_token: String,
    session_expires_at_ms: i64,
    members: Vec<Member>,
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock is before Unix epoch")
        .as_millis() as i64
}

fn random_bytes(count: usize) -> Vec<u8> {
    let mut bytes = vec![0_u8; count];
    OsRng.fill_bytes(&mut bytes);
    bytes
}

fn random_token(count: usize) -> String {
    URL_SAFE_NO_PAD.encode(random_bytes(count))
}

fn invitation_code() -> String {
    const ALPHABET: &[u8] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    let random = random_bytes(12);
    let mut code = String::with_capacity(14);
    for (index, value) in random.into_iter().enumerate() {
        if index == 4 || index == 8 { code.push('-'); }
        code.push(ALPHABET[value as usize % ALPHABET.len()] as char);
    }
    code
}

fn hex(bytes: &[u8]) -> String {
    const HEX: &[u8] = b"0123456789abcdef";
    let mut value = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        value.push(HEX[(byte >> 4) as usize] as char);
        value.push(HEX[(byte & 0x0f) as usize] as char);
    }
    value
}

fn encode_field(value: &str) -> String {
    const HEX: &[u8] = b"0123456789ABCDEF";
    let mut encoded = String::new();
    for byte in value.bytes() {
        if byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.' | b'~') {
            encoded.push(byte as char);
        } else {
            encoded.push('%');
            encoded.push(HEX[(byte >> 4) as usize] as char);
            encoded.push(HEX[(byte & 0x0f) as usize] as char);
        }
    }
    encoded
}

fn decode_field(value: &str) -> Result<String, ApiError> {
    fn digit(value: u8) -> Option<u8> {
        match value {
            b'0'..=b'9' => Some(value - b'0'),
            b'a'..=b'f' => Some(value - b'a' + 10),
            b'A'..=b'F' => Some(value - b'A' + 10),
            _ => None,
        }
    }
    let bytes = value.as_bytes();
    let mut decoded = Vec::with_capacity(bytes.len());
    let mut index = 0;
    while index < bytes.len() {
        if bytes[index] != b'%' {
            decoded.push(bytes[index]);
            index += 1;
            continue;
        }
        if index + 2 >= bytes.len() {
            return Err(ApiError::bad("malformed field escaping"));
        }
        let high = digit(bytes[index + 1]).ok_or_else(|| ApiError::bad("malformed field escaping"))?;
        let low = digit(bytes[index + 2]).ok_or_else(|| ApiError::bad("malformed field escaping"))?;
        decoded.push((high << 4) | low);
        index += 3;
    }
    String::from_utf8(decoded).map_err(|_| ApiError::bad("field is not UTF-8"))
}

fn fields(body: &[u8], expected: usize) -> Result<Vec<String>, ApiError> {
    if body.len() > MAX_BODY_BYTES {
        return Err(ApiError::bad("request body exceeds limit"));
    }
    let body = std::str::from_utf8(body).map_err(|_| ApiError::bad("request body is not UTF-8"))?;
    let parts: Vec<_> = body.split('\t').collect();
    if parts.len() != expected || parts.iter().any(|part| part.contains(['\r', '\n'])) {
        return Err(ApiError::bad("request fields are invalid"));
    }
    parts.into_iter().map(decode_field).collect()
}

fn header<'a>(headers: &'a HeaderMap, name: &str) -> Result<&'a str, ApiError> {
    headers
        .get(name)
        .and_then(|value| value.to_str().ok())
        .filter(|value| !value.is_empty())
        .ok_or_else(|| ApiError::unauthorized(format!("missing {name}")))
}

fn authenticate(
    state: &AppState,
    path: &str,
    headers: &HeaderMap,
    body: &[u8],
) -> Result<DeviceAuth, ApiError> {
    let device_id = header(headers, "x-veritassync-device-id")?.to_owned();
    let public_key_text = header(headers, "x-veritassync-public-key")?.to_owned();
    let timestamp_text = header(headers, "x-veritassync-timestamp")?;
    let nonce = header(headers, "x-veritassync-nonce")?;
    let signature_text = header(headers, "x-veritassync-signature")?;
    if nonce.len() > 80 || device_id.len() != 32 {
        return Err(ApiError::unauthorized("invalid device request metadata"));
    }
    let timestamp: i64 = timestamp_text.parse().map_err(|_| ApiError::unauthorized("invalid timestamp"))?;
    let current_seconds = now_ms() / 1000;
    if (current_seconds - timestamp).abs() > SIGNATURE_SKEW_SECONDS {
        return Err(ApiError::unauthorized("request timestamp is outside the accepted window"));
    }
    let public_key_bytes = URL_SAFE_NO_PAD
        .decode(&public_key_text)
        .map_err(|_| ApiError::unauthorized("invalid public key"))?;
    let public_key_array: [u8; 32] = public_key_bytes
        .try_into()
        .map_err(|_| ApiError::unauthorized("invalid public key size"))?;
    let fingerprint_bytes = blake3::hash(&public_key_array);
    let derived_device_id = hex(&fingerprint_bytes.as_bytes()[..16]);
    if derived_device_id != device_id {
        return Err(ApiError::unauthorized("device id does not match public key"));
    }
    let signature_bytes = URL_SAFE_NO_PAD
        .decode(signature_text)
        .map_err(|_| ApiError::unauthorized("invalid request signature"))?;
    let signature = Signature::from_slice(&signature_bytes)
        .map_err(|_| ApiError::unauthorized("invalid request signature size"))?;
    let body_hash = hex(blake3::hash(body).as_bytes());
    let canonical = format!("POST\n{path}\n{timestamp_text}\n{nonce}\n{body_hash}");
    VerifyingKey::from_bytes(&public_key_array)
        .map_err(|_| ApiError::unauthorized("invalid Ed25519 public key"))?
        .verify(canonical.as_bytes(), &signature)
        .map_err(|_| ApiError::unauthorized("request signature verification failed"))?;

    let database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    database.execute("DELETE FROM request_nonces WHERE expires_at_ms < ?1", [now_ms()])
        .map_err(|error| ApiError::internal(error.to_string()))?;
    let inserted = database.execute(
        "INSERT OR IGNORE INTO request_nonces(device_id, nonce, expires_at_ms) VALUES(?1, ?2, ?3)",
        params![device_id, nonce, now_ms() + SIGNATURE_SKEW_SECONDS * 2 * 1000],
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    if inserted != 1 {
        return Err(ApiError::unauthorized("request nonce was already used"));
    }
    Ok(DeviceAuth { device_id, public_key: public_key_text, fingerprint: hex(fingerprint_bytes.as_bytes()) })
}

fn role_valid(topology: &str, role: &str) -> bool {
    (topology == "one_way" && matches!(role, "source" | "target")) ||
        (topology == "bidirectional" && role == "peer")
}

fn member_count(transaction: &Transaction<'_>, room_id: &str) -> Result<i64, ApiError> {
    transaction.query_row(
        "SELECT COUNT(*) FROM members WHERE room_id=?1 AND revoked=0", [room_id], |row| row.get(0),
    ).map_err(|error| ApiError::internal(error.to_string()))
}

fn admit_member(transaction: &Transaction<'_>, room_id: &str, topology: &str,
                auth: &DeviceAuth, role: &str, joined_at_ms: i64) -> Result<(), ApiError> {
    if !role_valid(topology, role) { return Err(ApiError::forbidden("role is incompatible with room topology")); }
    if topology == "bidirectional" && member_count(transaction, room_id)? >= 2 {
        return Err(ApiError::conflict("bidirectional room already has two peers"));
    }
    if topology == "one_way" && role == "source" {
        let sources: i64 = transaction.query_row(
            "SELECT COUNT(*) FROM members WHERE room_id=?1 AND role='source' AND revoked=0",
            [room_id], |row| row.get(0),
        ).map_err(|error| ApiError::internal(error.to_string()))?;
        if sources > 0 { return Err(ApiError::conflict("one-way room already has a source")); }
    }
    transaction.execute(
        "INSERT INTO members(room_id, device_id, public_key, fingerprint, role, joined_at_ms, revoked) VALUES(?1, ?2, ?3, ?4, ?5, ?6, 0)",
        params![room_id, auth.device_id, auth.public_key, auth.fingerprint, role, joined_at_ms],
    ).map_err(|error| ApiError::conflict(format!("device cannot join room: {error}")))?;
    Ok(())
}

fn create_session(transaction: &Transaction<'_>, room_id: &str, device_id: &str,
                  created_at_ms: i64) -> Result<(String, i64), ApiError> {
    let token = random_token(32);
    let expires = created_at_ms + SESSION_TTL_MS;
    transaction.execute(
        "INSERT INTO sessions(token, room_id, device_id, expires_at_ms) VALUES(?1, ?2, ?3, ?4)",
        params![token, room_id, device_id, expires],
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    Ok((token, expires))
}

fn list_members(transaction: &Transaction<'_>, room_id: &str) -> Result<Vec<Member>, ApiError> {
    let mut statement = transaction.prepare(
        "SELECT device_id, fingerprint, role FROM members WHERE room_id=?1 AND revoked=0 ORDER BY device_id"
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    let rows = statement.query_map([room_id], |row| Ok(Member {
        device_id: row.get(0)?, fingerprint: row.get(1)?, role: row.get(2)?,
    })).map_err(|error| ApiError::internal(error.to_string()))?;
    rows.collect::<Result<Vec<_>, _>>().map_err(|error| ApiError::internal(error.to_string()))
}

fn enrollment_response(enrollment: Enrollment) -> String {
    let mut response = format!("OK\t{}\t{}\t{}\t{}\tMEMBERS\n",
        encode_field(&enrollment.room_id), encode_field(&enrollment.authorization_digest),
        encode_field(&enrollment.session_token), enrollment.session_expires_at_ms);
    for member in enrollment.members {
        response.push_str(&format!("MEMBER\t{}\t{}\t{}\n", encode_field(&member.device_id),
                                   encode_field(&member.fingerprint), member.role));
    }
    response.push_str("END\n");
    response
}

async fn create_room(State(state): State<AppState>, headers: HeaderMap, body: Bytes) -> ApiResult {
    let auth = authenticate(&state, "/v1/rooms/create", &headers, &body)?;
    let values = fields(&body, 4)?;
    let (task_id, topology, local_role, invited_role) = (&values[0], &values[1], &values[2], &values[3]);
    if task_id.is_empty() || !matches!(topology.as_str(), "one_way" | "bidirectional") ||
       !role_valid(topology, local_role) || !role_valid(topology, invited_role) ||
       (topology == "one_way" && (local_role != "source" || invited_role != "target")) {
        return Err(ApiError::bad("room topology and roles are invalid"));
    }
    let current = now_ms();
    let room_id = random_token(16);
    let code = invitation_code();
    let authorization_digest = hex(&random_bytes(32));
    let mut database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let transaction = database.transaction().map_err(|error| ApiError::internal(error.to_string()))?;
    transaction.execute(
        "INSERT INTO rooms(room_id, task_id, topology, authorization_digest, created_at_ms) VALUES(?1, ?2, ?3, ?4, ?5)",
        params![room_id, task_id, topology, authorization_digest, current],
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    admit_member(&transaction, &room_id, topology, &auth, local_role, current)?;
    transaction.execute(
        "INSERT INTO invitations(code, room_id, invited_role, expires_at_ms) VALUES(?1, ?2, ?3, ?4)",
        params![code, room_id, invited_role, current + INVITATION_TTL_MS],
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    let (session_token, session_expires_at_ms) = create_session(&transaction, &room_id, &auth.device_id, current)?;
    let members = list_members(&transaction, &room_id)?;
    transaction.commit().map_err(|error| ApiError::internal(error.to_string()))?;
    Ok(format!("INVITE\t{}\n{}", encode_field(&code), enrollment_response(Enrollment {
        room_id, authorization_digest, session_token, session_expires_at_ms, members,
    })))
}

async fn redeem_invitation(State(state): State<AppState>, headers: HeaderMap, body: Bytes) -> ApiResult {
    let auth = authenticate(&state, "/v1/invitations/redeem", &headers, &body)?;
    let values = fields(&body, 3)?;
    let (code, task_id, requested_role) = (&values[0], &values[1], &values[2]);
    let current = now_ms();
    let mut database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let transaction = database.transaction().map_err(|error| ApiError::internal(error.to_string()))?;
    let invitation: Option<(String, String, i64, Option<String>)> = transaction.query_row(
        "SELECT room_id, invited_role, expires_at_ms, redeemed_by FROM invitations WHERE code=?1",
        [code], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
    ).optional().map_err(|error| ApiError::internal(error.to_string()))?;
    let (room_id, invited_role, expires_at, redeemed_by) = invitation.ok_or_else(|| ApiError::bad("invitation does not exist"))?;
    if expires_at < current || redeemed_by.is_some() { return Err(ApiError::forbidden("invitation has expired or was redeemed")); }
    if requested_role != &invited_role { return Err(ApiError::forbidden("invitation role does not match")); }
    let (stored_task, topology, authorization_digest): (String, String, String) = transaction.query_row(
        "SELECT task_id, topology, authorization_digest FROM rooms WHERE room_id=?1", [&room_id],
        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    if &stored_task != task_id { return Err(ApiError::forbidden("invitation task does not match")); }
    admit_member(&transaction, &room_id, &topology, &auth, requested_role, current)?;
    transaction.execute("UPDATE invitations SET redeemed_by=?1 WHERE code=?2",
                        params![auth.device_id, code])
        .map_err(|error| ApiError::internal(error.to_string()))?;
    let (session_token, session_expires_at_ms) = create_session(&transaction, &room_id, &auth.device_id, current)?;
    let members = list_members(&transaction, &room_id)?;
    transaction.commit().map_err(|error| ApiError::internal(error.to_string()))?;
    Ok(enrollment_response(Enrollment { room_id, authorization_digest, session_token,
                                        session_expires_at_ms, members }))
}

async fn join_room(State(state): State<AppState>, headers: HeaderMap, body: Bytes) -> ApiResult {
    let auth = authenticate(&state, "/v1/rooms/join", &headers, &body)?;
    let values = fields(&body, 3)?;
    let (room_id, task_id, role) = (&values[0], &values[1], &values[2]);
    let current = now_ms();
    let mut database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let transaction = database.transaction().map_err(|error| ApiError::internal(error.to_string()))?;
    let room: Option<(String, String)> = transaction.query_row(
        "SELECT task_id, authorization_digest FROM rooms WHERE room_id=?1", [room_id],
        |row| Ok((row.get(0)?, row.get(1)?)),
    ).optional().map_err(|error| ApiError::internal(error.to_string()))?;
    let (stored_task, authorization_digest) = room.ok_or_else(|| ApiError::bad("room does not exist"))?;
    if &stored_task != task_id { return Err(ApiError::forbidden("room task does not match")); }
    let member: Option<(String, String, i64)> = transaction.query_row(
        "SELECT public_key, role, revoked FROM members WHERE room_id=?1 AND device_id=?2",
        params![room_id, auth.device_id], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
    ).optional().map_err(|error| ApiError::internal(error.to_string()))?;
    let (stored_key, stored_role, revoked) = member.ok_or_else(|| ApiError::forbidden("device is not a room member"))?;
    if stored_key != auth.public_key || &stored_role != role || revoked != 0 {
        return Err(ApiError::forbidden("room membership does not match device identity"));
    }
    let (session_token, session_expires_at_ms) = create_session(&transaction, room_id, &auth.device_id, current)?;
    let members = list_members(&transaction, room_id)?;
    transaction.commit().map_err(|error| ApiError::internal(error.to_string()))?;
    Ok(enrollment_response(Enrollment { room_id: room_id.clone(), authorization_digest,
                                        session_token, session_expires_at_ms, members }))
}

fn authorize_session(state: &AppState, headers: &HeaderMap, auth: &DeviceAuth,
                     room_id: &str) -> Result<(), ApiError> {
    let token = header(headers, "x-veritassync-session")?;
    let database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let valid: i64 = database.query_row(
        "SELECT COUNT(*) FROM sessions WHERE token=?1 AND room_id=?2 AND device_id=?3 AND expires_at_ms>=?4",
        params![token, room_id, auth.device_id, now_ms()], |row| row.get(0),
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    if valid != 1 { return Err(ApiError::unauthorized("Tracker session is invalid or expired")); }
    Ok(())
}

fn relay_allowed(topology: &str, sender_role: &str, recipient_role: &str, kind: &str) -> bool {
    if topology == "bidirectional" {
        return sender_role == "peer" && recipient_role == "peer";
    }
    let source_to_target = sender_role == "source" && recipient_role == "target";
    let target_to_source = sender_role == "target" && recipient_role == "source";
    (source_to_target && kind != "answer") ||
        (target_to_source && matches!(kind, "answer" | "ice"))
}

async fn send_signal(State(state): State<AppState>, headers: HeaderMap, body: Bytes) -> ApiResult {
    let auth = authenticate(&state, "/v1/signals/send", &headers, &body)?;
    let values = fields(&body, 6)?;
    let (room_id, kind, recipient, payload, candidate_mid, index_text) =
        (&values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    authorize_session(&state, &headers, &auth, room_id)?;
    if !matches!(kind.as_str(), "offer" | "answer" | "ice" | "ice_restart") ||
       payload.is_empty() || payload.len() > MAX_SIGNAL_BYTES || recipient == &auth.device_id {
        return Err(ApiError::bad("signal is invalid"));
    }
    let candidate_index: i32 = index_text.parse().map_err(|_| ApiError::bad("candidate index is invalid"))?;
    let database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let topology: String = database.query_row("SELECT topology FROM rooms WHERE room_id=?1", [room_id], |row| row.get(0))
        .map_err(|_| ApiError::forbidden("room does not exist"))?;
    let sender_role: String = database.query_row(
        "SELECT role FROM members WHERE room_id=?1 AND device_id=?2 AND revoked=0",
        params![room_id, auth.device_id], |row| row.get(0),
    ).map_err(|_| ApiError::forbidden("sender is not an active room member"))?;
    let recipient_role: String = database.query_row(
        "SELECT role FROM members WHERE room_id=?1 AND device_id=?2 AND revoked=0",
        params![room_id, recipient], |row| row.get(0),
    ).map_err(|_| ApiError::forbidden("recipient is not an active room member"))?;
    if !relay_allowed(&topology, &sender_role, &recipient_role, kind) {
        return Err(ApiError::forbidden("signal violates room topology"));
    }
    database.execute(
        "INSERT INTO signals(room_id, sender_device_id, recipient_device_id, kind, payload, candidate_mid, candidate_mline_index, created_at_ms) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        params![room_id, auth.device_id, recipient, kind, payload, candidate_mid, candidate_index, now_ms()],
    ).map_err(|error| ApiError::internal(error.to_string()))?;
    Ok("OK\n".to_string())
}

async fn drain_signals(State(state): State<AppState>, headers: HeaderMap, body: Bytes) -> ApiResult {
    let auth = authenticate(&state, "/v1/signals/drain", &headers, &body)?;
    let values = fields(&body, 1)?;
    let room_id = &values[0];
    authorize_session(&state, &headers, &auth, room_id)?;
    let mut database = state.database.lock().map_err(|_| ApiError::internal("database lock poisoned"))?;
    let transaction = database.transaction().map_err(|error| ApiError::internal(error.to_string()))?;
    let rows = {
        let mut statement = transaction.prepare(
            "SELECT signal_id, kind, sender_device_id, payload, candidate_mid, candidate_mline_index FROM signals WHERE room_id=?1 AND recipient_device_id=?2 ORDER BY signal_id LIMIT ?3"
        ).map_err(|error| ApiError::internal(error.to_string()))?;
        let mapped = statement.query_map(params![room_id, auth.device_id, MAX_DRAIN_SIGNALS], |row| {
            Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?, row.get::<_, String>(2)?,
                row.get::<_, String>(3)?, row.get::<_, String>(4)?, row.get::<_, i32>(5)?))
        }).map_err(|error| ApiError::internal(error.to_string()))?;
        mapped.collect::<Result<Vec<_>, _>>().map_err(|error| ApiError::internal(error.to_string()))?
    };
    if let Some((last_id, ..)) = rows.last() {
        transaction.execute(
            "DELETE FROM signals WHERE room_id=?1 AND recipient_device_id=?2 AND signal_id<=?3",
            params![room_id, auth.device_id, last_id],
        ).map_err(|error| ApiError::internal(error.to_string()))?;
    }
    transaction.commit().map_err(|error| ApiError::internal(error.to_string()))?;
    let mut response = "OK\n".to_string();
    for (_, kind, sender, payload, mid, index) in rows {
        response.push_str(&format!("SIGNAL\t{}\t{}\t{}\t{}\t{}\t{}\n", kind,
            encode_field(&sender), encode_field(&auth.device_id), encode_field(&payload),
            encode_field(&mid), index));
    }
    response.push_str("END\n");
    Ok(response)
}

fn initialize_database(path: impl AsRef<Path>) -> Result<Connection, rusqlite::Error> {
    let database = Connection::open(path)?;
    database.execute_batch(r#"
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=5000;
CREATE TABLE IF NOT EXISTS rooms (
  room_id TEXT PRIMARY KEY, task_id TEXT NOT NULL, topology TEXT NOT NULL,
  authorization_digest TEXT NOT NULL, created_at_ms INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS members (
  room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
  device_id TEXT NOT NULL, public_key TEXT NOT NULL, fingerprint TEXT NOT NULL,
  role TEXT NOT NULL, joined_at_ms INTEGER NOT NULL, revoked INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY(room_id, device_id)
);
CREATE TABLE IF NOT EXISTS invitations (
  code TEXT PRIMARY KEY, room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
  invited_role TEXT NOT NULL, expires_at_ms INTEGER NOT NULL, redeemed_by TEXT
);
CREATE TABLE IF NOT EXISTS sessions (
  token TEXT PRIMARY KEY, room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
  device_id TEXT NOT NULL, expires_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS sessions_member ON sessions(room_id, device_id, expires_at_ms);
CREATE TABLE IF NOT EXISTS signals (
  signal_id INTEGER PRIMARY KEY AUTOINCREMENT,
  room_id TEXT NOT NULL REFERENCES rooms(room_id) ON DELETE CASCADE,
  sender_device_id TEXT NOT NULL, recipient_device_id TEXT NOT NULL,
  kind TEXT NOT NULL, payload TEXT NOT NULL, candidate_mid TEXT NOT NULL,
  candidate_mline_index INTEGER NOT NULL, created_at_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS signals_inbox ON signals(room_id, recipient_device_id, signal_id);
CREATE TABLE IF NOT EXISTS request_nonces (
  device_id TEXT NOT NULL, nonce TEXT NOT NULL, expires_at_ms INTEGER NOT NULL,
  PRIMARY KEY(device_id, nonce)
);
CREATE INDEX IF NOT EXISTS request_nonces_expiry ON request_nonces(expires_at_ms);
"#)?;
    Ok(database)
}

fn app(state: AppState) -> Router {
    Router::new()
        .route("/healthz", get(|| async { "ok\n" }))
        .route("/v1/rooms/create", post(create_room))
        .route("/v1/invitations/redeem", post(redeem_invitation))
        .route("/v1/rooms/join", post(join_room))
        .route("/v1/signals/send", post(send_signal))
        .route("/v1/signals/drain", post(drain_signals))
        .with_state(state)
}

#[tokio::main]
async fn main() {
    let bind: SocketAddr = env::var("VERITASSYNC_TRACKER_BIND")
        .unwrap_or_else(|_| "127.0.0.1:8787".to_string())
        .parse()
        .expect("VERITASSYNC_TRACKER_BIND must be an IP socket address");
    let database_path = env::var("VERITASSYNC_TRACKER_DB")
        .unwrap_or_else(|_| "tracker.db".to_string());
    let state = AppState {
        database: Arc::new(Mutex::new(initialize_database(database_path).expect("cannot open Tracker database"))),
    };
    let listener = tokio::net::TcpListener::bind(bind).await.expect("cannot bind Tracker listener");
    println!("VeritasSync Tracker listening on {bind}");
    axum::serve(listener, app(state))
        .with_graceful_shutdown(async { let _ = tokio::signal::ctrl_c().await; })
        .await
        .expect("Tracker server failed");
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{body::Body, http::Request};
    use ed25519_dalek::{Signer, SigningKey};
    use http_body_util::BodyExt;
    use tower::ServiceExt;

    fn test_state() -> AppState {
        AppState { database: Arc::new(Mutex::new(initialize_database(":memory:").unwrap())) }
    }

    fn signed_request(path: &str, body: &str, key: &SigningKey, nonce: &str) -> Request<Body> {
        let public = key.verifying_key().to_bytes();
        let fingerprint = blake3::hash(&public);
        let device_id = hex(&fingerprint.as_bytes()[..16]);
        let timestamp = now_ms() / 1000;
        let canonical = format!("POST\n{path}\n{timestamp}\n{nonce}\n{}", hex(blake3::hash(body.as_bytes()).as_bytes()));
        let signature = key.sign(canonical.as_bytes());
        Request::post(path)
            .header("x-veritassync-device-id", device_id)
            .header("x-veritassync-public-key", URL_SAFE_NO_PAD.encode(public))
            .header("x-veritassync-timestamp", timestamp.to_string())
            .header("x-veritassync-nonce", nonce)
            .header("x-veritassync-signature", URL_SAFE_NO_PAD.encode(signature.to_bytes()))
            .body(Body::from(body.to_string())).unwrap()
    }

    #[tokio::test]
    async fn creates_and_redeems_signed_invitation() {
        let state = test_state();
        let creator = SigningKey::from_bytes(&[7_u8; 32]);
        let joiner = SigningKey::from_bytes(&[9_u8; 32]);
        let response = app(state.clone()).oneshot(signed_request(
            "/v1/rooms/create", "photos\tone_way\tsource\ttarget", &creator, "nonce-create",
        )).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        let text = std::str::from_utf8(&body).unwrap();
        let code = text.lines().next().unwrap().split('\t').nth(1).unwrap();
        let redeem_body = format!("{code}\tphotos\ttarget");
        let response = app(state).oneshot(signed_request(
            "/v1/invitations/redeem", &redeem_body, &joiner, "nonce-redeem",
        )).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        let body = response.into_body().collect().await.unwrap().to_bytes();
        let text = std::str::from_utf8(&body).unwrap();
        assert_eq!(text.lines().filter(|line| line.starts_with("MEMBER\t")).count(), 2);
    }

    #[tokio::test]
    async fn rejects_replayed_signed_request() {
        let state = test_state();
        let creator = SigningKey::from_bytes(&[3_u8; 32]);
        let request = || signed_request(
            "/v1/rooms/create", "docs\tone_way\tsource\ttarget", &creator, "same-nonce",
        );
        assert_eq!(app(state.clone()).oneshot(request()).await.unwrap().status(), StatusCode::OK);
        assert_eq!(app(state).oneshot(request()).await.unwrap().status(), StatusCode::UNAUTHORIZED);
    }
}
