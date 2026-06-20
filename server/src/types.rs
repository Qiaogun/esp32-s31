use chrono::{DateTime, Utc};
use serde::{Deserialize, Serialize};

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Emotion {
    Smile,
    Grump,
    Surprise,
    Squint,
    Sad,
    Blank,
    Upset,
    Blink,
    Cheeky,
    Frown,
}

impl Emotion {
    pub fn as_device_mood(self) -> &'static str {
        match self {
            Emotion::Smile => "smile",
            Emotion::Grump => "grump",
            Emotion::Surprise => "surprise",
            Emotion::Squint => "squint",
            Emotion::Sad => "sad",
            Emotion::Blank => "blank",
            Emotion::Upset => "upset",
            Emotion::Blink => "blink",
            Emotion::Cheeky => "cheeky",
            Emotion::Frown => "frown",
        }
    }
}

impl Default for Emotion {
    fn default() -> Self {
        Self::Smile
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct HeartbeatRequest {
    pub device_id: String,
    pub firmware_version: Option<String>,
    pub ip: Option<String>,
    pub battery_percent: Option<u8>,
    pub current_mood: Option<Emotion>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct WakeRequest {
    pub device_id: String,
    pub phrase: String,
    pub confidence: f32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct EmotionMapRequest {
    pub text: String,
    pub wake_confidence: Option<f32>,
    pub vision_hint: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct EmotionMapResponse {
    pub emotion: Emotion,
    pub device_mood: String,
    pub confidence: f32,
    pub reason: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DialogRequest {
    pub device_id: String,
    pub text: String,
    pub locale: Option<String>,
    pub context: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ServerAudio {
    pub mode: String,
    pub audio_url: Option<String>,
    pub text: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DeviceAction {
    pub kind: String,
    pub value: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DialogResponse {
    pub text: String,
    pub emotion: Emotion,
    pub device_mood: String,
    pub actions: Vec<DeviceAction>,
    pub server_audio: ServerAudio,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CameraFrameRequest {
    pub device_id: String,
    pub mime: Option<String>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub image_base64: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct CameraFrameMeta {
    pub device_id: String,
    pub mime: String,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub bytes: usize,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct OtaManifest {
    pub version: String,
    pub channel: String,
    pub firmware_url: String,
    pub sha256: String,
    pub size: u64,
    pub notes: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct OtaReportRequest {
    pub device_id: String,
    pub current_version: String,
    pub target_version: Option<String>,
    pub status: String,
    pub detail: Option<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct OtaRuntime {
    pub last_status: String,
    pub current_version: Option<String>,
    pub target_version: Option<String>,
    pub detail: Option<String>,
    pub updated_at: DateTime<Utc>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct AssistantRuntime {
    pub last_user_text: Option<String>,
    pub last_reply_text: Option<String>,
    pub last_emotion: Emotion,
    pub last_wake_phrase: Option<String>,
    pub last_wake_confidence: Option<f32>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct DeviceRuntime {
    pub device_id: String,
    pub online: bool,
    pub firmware_version: Option<String>,
    pub ip: Option<String>,
    pub battery_percent: Option<u8>,
    pub last_seen_at: DateTime<Utc>,
    pub assistant: AssistantRuntime,
    pub camera: Option<CameraFrameMeta>,
    pub ota: OtaRuntime,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct HealthResponse {
    pub ok: bool,
    pub service: String,
    pub version: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(tag = "type", content = "payload", rename_all = "snake_case")]
pub enum ServerEvent {
    State(DeviceRuntime),
    Wake(WakeRequest),
    Dialog(DialogResponse),
    Camera(CameraFrameMeta),
    Ota(OtaRuntime),
}
