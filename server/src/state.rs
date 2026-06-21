use std::{collections::VecDeque, sync::Arc};

use chrono::Utc;
use tokio::sync::{RwLock, broadcast};

use crate::llm::LlmClient;
use crate::types::{
    AssistantRuntime, CameraFrameMeta, DeviceAction, DeviceRuntime, Emotion, OtaRuntime,
    ServerEvent,
};

#[derive(Clone)]
pub struct AppState {
    pub runtime: Arc<RwLock<DeviceRuntime>>,
    pub latest_frame: Arc<RwLock<Option<LatestFrame>>>,
    pub command_queue: Arc<RwLock<VecDeque<PendingDeviceCommand>>>,
    pub events: broadcast::Sender<ServerEvent>,
    pub llm: LlmClient,
}

#[derive(Clone)]
pub struct LatestFrame {
    pub meta: CameraFrameMeta,
    pub bytes: Vec<u8>,
}

#[derive(Clone)]
pub struct PendingDeviceCommand {
    pub device_id: String,
    pub action: DeviceAction,
}

impl AppState {
    pub fn new() -> Self {
        let (events, _) = broadcast::channel(128);
        let now = Utc::now();
        Self {
            runtime: Arc::new(RwLock::new(DeviceRuntime {
                device_id: "ouo-s31-korvo-1".to_string(),
                online: false,
                firmware_version: None,
                ip: None,
                battery_percent: None,
                last_seen_at: now,
                assistant: AssistantRuntime {
                    last_user_text: None,
                    last_reply_text: None,
                    last_emotion: Emotion::Smile,
                    last_wake_phrase: None,
                    last_wake_confidence: None,
                },
                camera: None,
                ota: OtaRuntime {
                    last_status: "idle".to_string(),
                    current_version: None,
                    target_version: None,
                    detail: None,
                    updated_at: now,
                },
            })),
            latest_frame: Arc::new(RwLock::new(None)),
            command_queue: Arc::new(RwLock::new(VecDeque::new())),
            events,
            llm: LlmClient::from_env(),
        }
    }

    pub fn publish(&self, event: ServerEvent) {
        let _ = self.events.send(event);
    }
}
