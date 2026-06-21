use anyhow::{Context, Result};
use reqwest::Client;
use serde::{Deserialize, Serialize};

#[derive(Clone)]
pub struct LlmClient {
    http: Client,
    base_url: String,
    model: String,
}

impl LlmClient {
    pub fn from_env() -> Self {
        let base_url = std::env::var("OUO_LLM_BASE_URL")
            .unwrap_or_else(|_| "http://127.0.0.1:11434/v1/chat/completions".to_string());
        let model = std::env::var("OUO_LLM_MODEL").unwrap_or_else(|_| "qwen2.5:7b".to_string());
        Self {
            http: Client::new(),
            base_url,
            model,
        }
    }

    #[cfg(test)]
    pub fn unavailable_for_tests() -> Self {
        Self {
            http: Client::builder()
                .timeout(std::time::Duration::from_millis(300))
                .build()
                .expect("test reqwest client"),
            base_url: "http://127.0.0.1:9/v1/chat/completions".to_string(),
            model: "test-unavailable".to_string(),
        }
    }

    pub async fn reply(&self, user_text: &str, context: Option<&str>) -> Result<String> {
        let system = "你是一个家庭 AI 设备的大脑。回答要短、自然、可执行；能控制家居时给出明确动作，但不要假装已经执行未知设备。";
        let prompt = match context {
            Some(context) if !context.trim().is_empty() => {
                format!("上下文：{}\n用户：{}", context.trim(), user_text.trim())
            }
            _ => user_text.trim().to_string(),
        };

        let req = ChatCompletionRequest {
            model: self.model.clone(),
            messages: vec![
                ChatMessage {
                    role: "system".to_string(),
                    content: system.to_string(),
                },
                ChatMessage {
                    role: "user".to_string(),
                    content: prompt,
                },
            ],
            temperature: 0.6,
            stream: false,
        };

        let response = self
            .http
            .post(&self.base_url)
            .json(&req)
            .send()
            .await
            .context("local llm request failed")?
            .error_for_status()
            .context("local llm returned an error status")?
            .json::<ChatCompletionResponse>()
            .await
            .context("local llm response was not OpenAI-compatible JSON")?;

        Ok(response
            .choices
            .first()
            .map(|choice| choice.message.content.trim().to_string())
            .filter(|text| !text.is_empty())
            .unwrap_or_else(|| "我听到了，但本地模型没有返回内容。".to_string()))
    }
}

#[derive(Debug, Serialize)]
struct ChatCompletionRequest {
    model: String,
    messages: Vec<ChatMessage>,
    temperature: f32,
    stream: bool,
}

#[derive(Debug, Deserialize, Serialize)]
struct ChatMessage {
    role: String,
    content: String,
}

#[derive(Debug, Deserialize)]
struct ChatCompletionResponse {
    choices: Vec<ChatChoice>,
}

#[derive(Debug, Deserialize)]
struct ChatChoice {
    message: ChatMessage,
}
