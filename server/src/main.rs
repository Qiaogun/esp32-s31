mod api;
mod emotion;
mod llm;
mod state;
mod types;

use std::net::SocketAddr;

use anyhow::Result;
use tower_http::{services::ServeDir, trace::TraceLayer};
use tracing_subscriber::{layer::SubscriberExt, util::SubscriberInitExt};

use crate::state::AppState;

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::registry()
        .with(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "ouo_ai_home_server=info,tower_http=info,axum=info".into()),
        )
        .with(tracing_subscriber::fmt::layer())
        .init();

    let state = AppState::new();
    let app = api::router(state)
        .nest_service("/ota", ServeDir::new("server/ota"))
        .fallback_service(ServeDir::new("server/static").append_index_html_on_directories(true))
        .layer(TraceLayer::new_for_http());

    let addr: SocketAddr = std::env::var("OUO_SERVER_ADDR")
        .unwrap_or_else(|_| "0.0.0.0:8787".to_string())
        .parse()?;

    tracing::info!("ouo ai home server listening on http://{addr}");
    let listener = tokio::net::TcpListener::bind(addr).await?;
    axum::serve(listener, app).await?;
    Ok(())
}
