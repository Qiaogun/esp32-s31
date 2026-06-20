const stateEl = document.querySelector("#state");
const statusEl = document.querySelector("#status");
const eventsEl = document.querySelector("#events");
const cameraEl = document.querySelector("#camera");
const cameraMetaEl = document.querySelector("#camera-meta");
const replyEl = document.querySelector("#reply");
const dialogForm = document.querySelector("#dialog-form");
const dialogText = document.querySelector("#dialog-text");
const speakToggle = document.querySelector("#speak-toggle");
const refreshCamera = document.querySelector("#refresh-camera");

let speechEnabled = true;

function pretty(value) {
  return JSON.stringify(value, null, 2);
}

async function loadState() {
  const res = await fetch("/api/v1/state");
  const state = await res.json();
  stateEl.textContent = pretty(state);
  statusEl.textContent = `${state.device_id} · ${state.online ? "online" : "offline"} · mood ${state.assistant.last_emotion}`;
}

async function loadCamera() {
  const res = await fetch(`/api/v1/camera/latest?ts=${Date.now()}`);
  if (!res.ok) {
    cameraMetaEl.textContent = "No frame yet";
    return;
  }
  const blob = await res.blob();
  cameraEl.src = URL.createObjectURL(blob);
  cameraMetaEl.textContent = `${blob.type || "image"} · ${blob.size} bytes · ${new Date().toLocaleTimeString()}`;
}

function pushEvent(event) {
  const item = document.createElement("li");
  item.textContent = `${new Date().toLocaleTimeString()} ${JSON.stringify(event)}`;
  eventsEl.prepend(item);
  while (eventsEl.children.length > 80) {
    eventsEl.lastElementChild.remove();
  }
}

function speak(text) {
  if (!speechEnabled || !("speechSynthesis" in window)) {
    return;
  }
  window.speechSynthesis.cancel();
  const utterance = new SpeechSynthesisUtterance(text);
  utterance.lang = "zh-CN";
  window.speechSynthesis.speak(utterance);
}

function connectEvents() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/api/v1/events`);
  ws.onopen = () => {
    statusEl.textContent = "event stream connected";
  };
  ws.onmessage = (message) => {
    const event = JSON.parse(message.data);
    pushEvent(event);
    if (event.type === "state") {
      stateEl.textContent = pretty(event.payload);
    }
    if (event.type === "camera") {
      loadCamera();
    }
    if (event.type === "dialog") {
      replyEl.textContent = event.payload.text;
      speak(event.payload.server_audio.text);
    }
  };
  ws.onclose = () => {
    statusEl.textContent = "event stream reconnecting...";
    setTimeout(connectEvents, 1200);
  };
}

dialogForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const text = dialogText.value.trim();
  if (!text) {
    return;
  }
  replyEl.textContent = "thinking...";
  const res = await fetch("/api/v1/dialog", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ device_id: "ouo-s31-korvo-1", text, locale: "zh-CN" }),
  });
  const data = await res.json();
  replyEl.textContent = data.text;
  speak(data.server_audio.text);
  await loadState();
});

speakToggle.addEventListener("click", () => {
  speechEnabled = !speechEnabled;
  speakToggle.textContent = speechEnabled ? "Server speech on" : "Server speech off";
});

refreshCamera.addEventListener("click", loadCamera);

loadState();
loadCamera();
connectEvents();
