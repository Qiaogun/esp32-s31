use crate::types::{Emotion, EmotionMapRequest, EmotionMapResponse};

pub fn map_emotion(input: &EmotionMapRequest) -> EmotionMapResponse {
    let haystack = format!(
        "{} {}",
        input.text.to_lowercase(),
        input.vision_hint.clone().unwrap_or_default().to_lowercase()
    );

    let (emotion, reason) = if contains_any(
        &haystack,
        &["惊喜", "厉害", "太棒", "surprise", "wow", "amazing"],
    ) {
        (Emotion::Surprise, "detected surprise or delight")
    } else if contains_any(&haystack, &["难过", "伤心", "sad", "lonely", "失落"]) {
        (Emotion::Sad, "detected sadness")
    } else if contains_any(
        &haystack,
        &["生气", "烦", "angry", "annoyed", "坏了", "失败"],
    ) {
        (Emotion::Grump, "detected frustration")
    } else if contains_any(&haystack, &["调皮", "玩笑", "嘿嘿", "joke", "funny"]) {
        (Emotion::Cheeky, "detected playful intent")
    } else if contains_any(&haystack, &["困", "累", "sleepy", "tired"]) {
        (Emotion::Squint, "detected low energy")
    } else if contains_any(&haystack, &["担心", "糟糕", "upset", "worried"]) {
        (Emotion::Upset, "detected worry")
    } else if input.wake_confidence.unwrap_or_default() > 0.82 {
        (Emotion::Blink, "high confidence wake acknowledgement")
    } else {
        (Emotion::Smile, "neutral friendly baseline")
    };

    EmotionMapResponse {
        emotion,
        device_mood: emotion.as_device_mood().to_string(),
        confidence: confidence_for(emotion, input),
        reason: reason.to_string(),
    }
}

fn contains_any(value: &str, needles: &[&str]) -> bool {
    needles.iter().any(|needle| value.contains(needle))
}

fn confidence_for(emotion: Emotion, input: &EmotionMapRequest) -> f32 {
    if emotion == Emotion::Smile {
        0.58
    } else {
        input.wake_confidence.unwrap_or(0.74).clamp(0.55, 0.96)
    }
}
