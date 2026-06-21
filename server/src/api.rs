use axum::{
    Json, Router,
    extract::{
        Query, State,
        ws::{Message, WebSocket, WebSocketUpgrade},
    },
    http::{HeaderMap, HeaderValue, StatusCode, header},
    response::{IntoResponse, Response},
    routing::{get, post},
};
use base64::{Engine, engine::general_purpose::STANDARD};
use chrono::Utc;
use serde::Deserialize;
use serde_json::json;
use std::path::Path;

use crate::emotion::map_emotion;
use crate::state::{AppState, LatestFrame, PendingDeviceCommand};
use crate::types::{
    CameraFrameMeta, CameraFrameRequest, DeviceAction, DeviceCommandRequest, DeviceCommandResponse,
    DialogRequest, DialogResponse, EmotionMapRequest, HealthResponse, HeartbeatRequest,
    OtaManifest, OtaReportRequest, OtaRuntime, ServerAudio, ServerEvent, WakeRequest,
};

pub fn router(state: AppState) -> Router {
    Router::new()
        .route("/health", get(health))
        .route("/api/v1/state", get(state_snapshot))
        .route("/api/v1/device/heartbeat", post(heartbeat))
        .route("/api/v1/device/command", post(device_command))
        .route("/api/v1/wake", post(wake))
        .route("/api/v1/emotion/map", post(emotion_map))
        .route("/api/v1/dialog", post(dialog))
        .route("/api/v1/camera/frame", post(camera_frame))
        .route("/api/v1/camera/latest", get(camera_latest))
        .route("/api/v1/ota/manifest", get(ota_manifest))
        .route("/api/v1/ota/report", post(ota_report))
        .route("/api/v1/events", get(events_ws))
        .with_state(state)
}

async fn health() -> Json<HealthResponse> {
    Json(HealthResponse {
        ok: true,
        service: "ouo-ai-home-server".to_string(),
        version: env!("CARGO_PKG_VERSION").to_string(),
    })
}

async fn state_snapshot(State(state): State<AppState>) -> Json<crate::types::DeviceRuntime> {
    Json(state.runtime.read().await.clone())
}

async fn heartbeat(
    State(state): State<AppState>,
    Json(req): Json<HeartbeatRequest>,
) -> Json<crate::types::DeviceRuntime> {
    let snapshot = {
        let mut runtime = state.runtime.write().await;
        runtime.device_id = req.device_id;
        runtime.online = true;
        runtime.firmware_version = req.firmware_version;
        runtime.ip = req.ip;
        runtime.battery_percent = req.battery_percent;
        runtime.last_seen_at = Utc::now();
        if let Some(mood) = req.current_mood {
            runtime.assistant.last_emotion = mood;
        }
        runtime.clone()
    };
    state.publish(ServerEvent::State(snapshot.clone()));
    Json(snapshot)
}

async fn device_command(
    State(state): State<AppState>,
    Json(req): Json<DeviceCommandRequest>,
) -> Result<Json<DeviceCommandResponse>, (StatusCode, Json<serde_json::Value>)> {
    if let Some(kind) = req.kind {
        let value = req.value.unwrap_or_default();
        if kind.trim().is_empty() || value.trim().is_empty() {
            return Err((
                StatusCode::BAD_REQUEST,
                Json(json!({ "error": "kind and value are required when enqueueing a command" })),
            ));
        }
        let mut queue = state.command_queue.write().await;
        queue.push_back(crate::state::PendingDeviceCommand {
            device_id: req.device_id.clone(),
            action: DeviceAction { kind, value },
        });
        let response = DeviceCommandResponse {
            device_id: req.device_id,
            actions: Vec::new(),
            queued: queue.len(),
        };
        state.publish(ServerEvent::Command(response.clone()));
        return Ok(Json(response));
    }

    let (action, queued) = {
        let mut queue = state.command_queue.write().await;
        let index = queue
            .iter()
            .position(|cmd| cmd.device_id == req.device_id || cmd.device_id == "*");
        let action = index.and_then(|idx| queue.remove(idx).map(|cmd| cmd.action));
        (action, queue.len())
    };
    let response = DeviceCommandResponse {
        device_id: req.device_id,
        actions: action.into_iter().collect(),
        queued,
    };
    state.publish(ServerEvent::Command(response.clone()));
    Ok(Json(response))
}

async fn wake(
    State(state): State<AppState>,
    Json(req): Json<WakeRequest>,
) -> Json<crate::types::DeviceRuntime> {
    let snapshot = {
        let mut runtime = state.runtime.write().await;
        runtime.device_id = req.device_id.clone();
        runtime.online = true;
        runtime.last_seen_at = Utc::now();
        runtime.assistant.last_wake_phrase = Some(req.phrase.clone());
        runtime.assistant.last_wake_confidence = Some(req.confidence);
        runtime.assistant.last_emotion = if req.confidence > 0.80 {
            crate::types::Emotion::Blink
        } else {
            crate::types::Emotion::Surprise
        };
        runtime.clone()
    };
    let mood_action = DeviceAction {
        kind: "set_mood".to_string(),
        value: snapshot.assistant.last_emotion.as_device_mood().to_string(),
    };
    let queued = {
        let mut queue = state.command_queue.write().await;
        queue.push_back(PendingDeviceCommand {
            device_id: req.device_id.clone(),
            action: mood_action,
        });
        queue.len()
    };
    state.publish(ServerEvent::Wake(req.clone()));
    state.publish(ServerEvent::Command(DeviceCommandResponse {
        device_id: req.device_id,
        actions: Vec::new(),
        queued,
    }));
    state.publish(ServerEvent::State(snapshot.clone()));
    Json(snapshot)
}

async fn emotion_map(Json(req): Json<EmotionMapRequest>) -> Json<crate::types::EmotionMapResponse> {
    Json(map_emotion(&req))
}

async fn dialog(
    State(state): State<AppState>,
    Json(req): Json<DialogRequest>,
) -> Json<DialogResponse> {
    let reply = match state.llm.reply(&req.text, req.context.as_deref()).await {
        Ok(text) => text,
        Err(err) => format!("本地模型暂时不可用：{err:#}"),
    };
    let mapped = map_emotion(&EmotionMapRequest {
        text: format!("{} {}", req.text, reply),
        wake_confidence: Some(0.80),
        vision_hint: None,
    });
    let response = DialogResponse {
        text: reply.clone(),
        emotion: mapped.emotion,
        device_mood: mapped.device_mood,
        actions: vec![DeviceAction {
            kind: "set_mood".to_string(),
            value: mapped.emotion.as_device_mood().to_string(),
        }],
        server_audio: ServerAudio {
            mode: "server_browser_speech".to_string(),
            audio_url: None,
            text: reply,
        },
    };

    let snapshot = {
        let mut runtime = state.runtime.write().await;
        runtime.device_id = req.device_id;
        runtime.online = true;
        runtime.last_seen_at = Utc::now();
        runtime.assistant.last_user_text = Some(req.text);
        runtime.assistant.last_reply_text = Some(response.text.clone());
        runtime.assistant.last_emotion = response.emotion;
        runtime.clone()
    };
    state.publish(ServerEvent::Dialog(response.clone()));
    state.publish(ServerEvent::State(snapshot));
    Json(response)
}

async fn camera_frame(
    State(state): State<AppState>,
    Json(req): Json<CameraFrameRequest>,
) -> Result<Json<CameraFrameMeta>, (StatusCode, Json<serde_json::Value>)> {
    let bytes = STANDARD
        .decode(req.image_base64.as_bytes())
        .map_err(|err| {
            (
                StatusCode::BAD_REQUEST,
                Json(json!({ "error": err.to_string() })),
            )
        })?;
    let meta = CameraFrameMeta {
        device_id: req.device_id,
        mime: req.mime.unwrap_or_else(|| "image/jpeg".to_string()),
        width: req.width,
        height: req.height,
        bytes: bytes.len(),
        updated_at: Utc::now(),
    };
    {
        let mut latest = state.latest_frame.write().await;
        *latest = Some(LatestFrame {
            meta: meta.clone(),
            bytes,
        });
    }
    let snapshot = {
        let mut runtime = state.runtime.write().await;
        runtime.camera = Some(meta.clone());
        runtime.last_seen_at = Utc::now();
        runtime.clone()
    };
    state.publish(ServerEvent::Camera(meta.clone()));
    state.publish(ServerEvent::State(snapshot));
    Ok(Json(meta))
}

async fn camera_latest(State(state): State<AppState>) -> Response {
    let latest = state.latest_frame.read().await.clone();
    let Some(frame) = latest else {
        return (StatusCode::NOT_FOUND, "no camera frame has been uploaded").into_response();
    };

    let mut headers = HeaderMap::new();
    headers.insert(
        header::CONTENT_TYPE,
        HeaderValue::from_str(&frame.meta.mime).unwrap_or(HeaderValue::from_static("image/jpeg")),
    );
    headers.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    (headers, frame.bytes).into_response()
}

#[derive(Debug, Deserialize)]
struct OtaQuery {
    channel: Option<String>,
}

async fn ota_manifest(
    Query(query): Query<OtaQuery>,
) -> Result<Json<OtaManifest>, (StatusCode, Json<serde_json::Value>)> {
    let manifest_path = std::env::var("OUO_OTA_MANIFEST").unwrap_or_else(|_| {
        if Path::new("server/ota/manifest.json").is_file() {
            "server/ota/manifest.json".to_string()
        } else {
            "ota/manifest.json".to_string()
        }
    });
    let bytes = tokio::fs::read_to_string(&manifest_path)
        .await
        .map_err(|err| {
            (
                StatusCode::NOT_FOUND,
                Json(json!({ "error": format!("cannot read manifest {manifest_path}: {err}") })),
            )
        })?;
    let mut manifest: OtaManifest = serde_json::from_str(&bytes).map_err(|err| {
        (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({ "error": format!("invalid ota manifest: {err}") })),
        )
    })?;
    if let Some(channel) = query.channel {
        manifest.channel = channel;
    }
    Ok(Json(manifest))
}

async fn ota_report(
    State(state): State<AppState>,
    Json(req): Json<OtaReportRequest>,
) -> Json<OtaRuntime> {
    let ota = OtaRuntime {
        last_status: req.status,
        current_version: Some(req.current_version),
        target_version: req.target_version,
        detail: req.detail,
        updated_at: Utc::now(),
    };
    {
        let mut runtime = state.runtime.write().await;
        runtime.device_id = req.device_id;
        runtime.ota = ota.clone();
        runtime.last_seen_at = Utc::now();
    }
    state.publish(ServerEvent::Ota(ota.clone()));
    Json(ota)
}

async fn events_ws(ws: WebSocketUpgrade, State(state): State<AppState>) -> impl IntoResponse {
    ws.on_upgrade(move |socket| events_socket(socket, state))
}

async fn events_socket(mut socket: WebSocket, state: AppState) {
    let mut receiver = state.events.subscribe();
    while let Ok(event) = receiver.recv().await {
        let Ok(text) = serde_json::to_string(&event) else {
            continue;
        };
        if socket.send(Message::Text(text.into())).await.is_err() {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{
        body::{Body, to_bytes},
        http::{Method, Request},
    };
    use serde::de::DeserializeOwned;
    use tower::ServiceExt;

    use crate::{llm::LlmClient, types::DeviceRuntime};

    fn test_app() -> Router {
        let mut state = AppState::new();
        state.llm = LlmClient::unavailable_for_tests();
        router(state)
    }

    async fn get_json<T: DeserializeOwned>(app: Router, uri: &str) -> (StatusCode, T) {
        request_json(app, Method::GET, uri, None).await
    }

    async fn post_json<T: DeserializeOwned>(
        app: Router,
        uri: &str,
        body: serde_json::Value,
    ) -> (StatusCode, T) {
        request_json(app, Method::POST, uri, Some(body)).await
    }

    async fn request_json<T: DeserializeOwned>(
        app: Router,
        method: Method,
        uri: &str,
        body: Option<serde_json::Value>,
    ) -> (StatusCode, T) {
        let mut builder = Request::builder().method(method).uri(uri);
        if body.is_some() {
            builder = builder.header(header::CONTENT_TYPE, "application/json");
        }
        let request_body = body.map_or_else(Body::empty, |value| Body::from(value.to_string()));
        let response = app
            .oneshot(builder.body(request_body).expect("request body"))
            .await
            .expect("router response");
        let status = response.status();
        let bytes = to_bytes(response.into_body(), usize::MAX)
            .await
            .expect("response body");
        let parsed = serde_json::from_slice::<T>(&bytes).unwrap_or_else(|err| {
            panic!(
                "failed to parse JSON response from {uri}: {err}; body={}",
                String::from_utf8_lossy(&bytes)
            )
        });
        (status, parsed)
    }

    #[tokio::test]
    async fn heartbeat_and_dialog_return_firmware_parseable_json() {
        let app = test_app();

        let (status, health): (StatusCode, HealthResponse) = get_json(app.clone(), "/health").await;
        assert_eq!(status, StatusCode::OK);
        assert!(health.ok);
        assert_eq!(health.service, "ouo-ai-home-server");

        let (status, heartbeat): (StatusCode, DeviceRuntime) = post_json(
            app.clone(),
            "/api/v1/device/heartbeat",
            json!({
                "device_id": "ouo-s31-test",
                "firmware_version": "0.2.0-test",
                "ip": "192.168.1.23",
                "battery_percent": null,
                "current_mood": "smile"
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(heartbeat.device_id, "ouo-s31-test");
        assert!(heartbeat.online);
        assert_eq!(
            heartbeat.assistant.last_emotion,
            crate::types::Emotion::Smile
        );

        let (status, dialog): (StatusCode, DialogResponse) = post_json(
            app,
            "/api/v1/dialog",
            json!({
                "device_id": "ouo-s31-test",
                "text": "你好",
                "locale": "zh-CN",
                "context": "protocol test"
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert!(!dialog.text.trim().is_empty());
        assert!(!dialog.device_mood.trim().is_empty());
        assert!(!dialog.actions.is_empty());
        assert_eq!(dialog.actions[0].kind, "set_mood");
        assert_eq!(dialog.actions[0].value, dialog.device_mood);
    }

    #[tokio::test]
    async fn device_command_queue_is_addressed_and_fifo() {
        let app = test_app();

        let (status, queued): (StatusCode, DeviceCommandResponse) = post_json(
            app.clone(),
            "/api/v1/device/command",
            json!({
                "device_id": "device-a",
                "kind": "set_mood",
                "value": "sad"
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(queued.actions.len(), 0);
        assert_eq!(queued.queued, 1);

        let (status, empty): (StatusCode, DeviceCommandResponse) = post_json(
            app.clone(),
            "/api/v1/device/command",
            json!({ "device_id": "device-b" }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert!(empty.actions.is_empty());
        assert_eq!(empty.queued, 1);

        let (status, polled): (StatusCode, DeviceCommandResponse) = post_json(
            app.clone(),
            "/api/v1/device/command",
            json!({ "device_id": "device-a" }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(polled.actions.len(), 1);
        assert_eq!(polled.actions[0].kind, "set_mood");
        assert_eq!(polled.actions[0].value, "sad");
        assert_eq!(polled.queued, 0);

        let (status, _queued_wildcard): (StatusCode, DeviceCommandResponse) = post_json(
            app.clone(),
            "/api/v1/device/command",
            json!({
                "device_id": "*",
                "kind": "capture_camera",
                "value": "snapshot"
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);

        let (status, wildcard): (StatusCode, DeviceCommandResponse) = post_json(
            app,
            "/api/v1/device/command",
            json!({ "device_id": "device-b" }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(wildcard.actions.len(), 1);
        assert_eq!(wildcard.actions[0].kind, "capture_camera");
        assert_eq!(wildcard.actions[0].value, "snapshot");
    }

    #[tokio::test]
    async fn wake_queues_mapped_mood_for_device_poll() {
        let app = test_app();

        let (status, wake): (StatusCode, DeviceRuntime) = post_json(
            app.clone(),
            "/api/v1/wake",
            json!({
                "device_id": "device-wake",
                "phrase": "ouo",
                "confidence": 0.91
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(wake.device_id, "device-wake");
        assert_eq!(wake.assistant.last_wake_phrase.as_deref(), Some("ouo"));
        assert_eq!(wake.assistant.last_emotion, crate::types::Emotion::Blink);

        let (status, polled): (StatusCode, DeviceCommandResponse) = post_json(
            app,
            "/api/v1/device/command",
            json!({ "device_id": "device-wake" }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(polled.actions.len(), 1);
        assert_eq!(polled.actions[0].kind, "set_mood");
        assert_eq!(polled.actions[0].value, "blink");
        assert_eq!(polled.queued, 0);
    }

    #[tokio::test]
    async fn camera_and_ota_endpoints_keep_device_contract() {
        let app = test_app();

        let response = app
            .clone()
            .oneshot(
                Request::builder()
                    .method(Method::GET)
                    .uri("/api/v1/camera/latest")
                    .body(Body::empty())
                    .expect("request body"),
            )
            .await
            .expect("router response");
        assert_eq!(response.status(), StatusCode::NOT_FOUND);

        let bmp_1x1 =
            "Qk06AAAAAAAAADYAAAAoAAAAAQAAAP////8BABgAAAAAAAQAAAATCwAAEwsAAAAAAAAAAAAA////AA==";
        let (status, frame): (StatusCode, CameraFrameMeta) = post_json(
            app.clone(),
            "/api/v1/camera/frame",
            json!({
                "device_id": "ouo-s31-test",
                "mime": "image/bmp",
                "width": 1,
                "height": 1,
                "image_base64": bmp_1x1
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(frame.device_id, "ouo-s31-test");
        assert_eq!(frame.mime, "image/bmp");
        assert!(frame.bytes > 0);

        let latest = app
            .clone()
            .oneshot(
                Request::builder()
                    .method(Method::GET)
                    .uri("/api/v1/camera/latest")
                    .body(Body::empty())
                    .expect("request body"),
            )
            .await
            .expect("router response");
        assert_eq!(latest.status(), StatusCode::OK);
        assert_eq!(
            latest.headers().get(header::CONTENT_TYPE).unwrap(),
            "image/bmp"
        );
        let latest_bytes = to_bytes(latest.into_body(), usize::MAX)
            .await
            .expect("camera bytes");
        assert_eq!(latest_bytes.len(), frame.bytes);

        let (status, manifest): (StatusCode, OtaManifest) =
            get_json(app.clone(), "/api/v1/ota/manifest?channel=test").await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(manifest.channel, "test");
        assert!(!manifest.version.trim().is_empty());
        assert!(!manifest.firmware_url.trim().is_empty());
        assert!(!manifest.sha256.trim().is_empty());
        assert!(manifest.size > 0);

        let (status, ota): (StatusCode, OtaRuntime) = post_json(
            app,
            "/api/v1/ota/report",
            json!({
                "device_id": "ouo-s31-test",
                "current_version": "0.2.0-test",
                "target_version": manifest.version,
                "status": "test_ok",
                "detail": "api unit test"
            }),
        )
        .await;
        assert_eq!(status, StatusCode::OK);
        assert_eq!(ota.last_status, "test_ok");
        assert_eq!(ota.current_version.as_deref(), Some("0.2.0-test"));
    }
}
