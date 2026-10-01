<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import Hls from "hls.js";

const tab = ref("multiview");
const info = ref(null);
const channels = ref([]);
const nmos = ref(null);
const config = ref(null);
const layout = ref("auto");
const expanded = ref(0);
const message = ref("");
const messageOk = ref(true);
const configText = ref("");
const envText = ref("");
const players = new Map();

const count = computed(() => channels.value.length);
const cols = computed(() => {
  if (layout.value !== "auto") return Number(layout.value);
  if (count.value <= 1) return 1;
  if (count.value <= 4) return 2;
  if (count.value <= 9) return 3;
  return 4;
});

function pageUrl(url) {
  try {
    const parsed = new URL(url);
    parsed.hostname = location.hostname;
    return parsed.toString();
  } catch {
    return url;
  }
}

function meterHeight(db) {
  const clamped = Math.max(-60, Math.min(0, Number(db) || -60));
  return `${((clamped + 60) / 60) * 100}%`;
}

function selectedPair(channel) {
  const pair = channel.audio_pair || 1;
  return [(pair - 1) * 2 + 1, (pair - 1) * 2 + 2];
}

async function load() {
  const [infoRes, channelRes, nmosRes, configRes, envRes] = await Promise.all([
    fetch("/api/v1/info"),
    fetch("/api/v1/channels"),
    fetch("/api/v1/nmos"),
    fetch("/api/v1/config"),
    fetch("/api/v1/config.env"),
  ]);
  info.value = await infoRes.json();
  channels.value = (await channelRes.json()).channels || [];
  nmos.value = await nmosRes.json();
  config.value = await configRes.json();
  if (!configText.value) configText.value = JSON.stringify(config.value.values || {}, null, 2);
  envText.value = await envRes.text();
}

function applyStatus(payload) {
  if (payload && payload.channels) channels.value = payload.channels;
}

function connectEvents() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const socket = new WebSocket(`${proto}://${location.host}/api/v1/events`);
  socket.onmessage = (event) => {
    try {
      applyStatus(JSON.parse(event.data));
    } catch {
      /* ignore malformed frames */
    }
  };
  socket.onclose = () => setTimeout(connectEvents, 1000);
  return socket;
}

async function playTile(channel, video) {
  if (!video) return;
  stopTile(channel.index);
  video.muted = true;
  const whep = pageUrl(channel.playback.whep);
  const hlsUrl = pageUrl(channel.playback.hls);
  try {
    const pc = new RTCPeerConnection();
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });
    const stream = new MediaStream();
    pc.ontrack = (ev) => {
      stream.addTrack(ev.track);
      video.srcObject = stream;
    };
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await new Promise((resolve) => {
      if (pc.iceGatheringState === "complete") resolve();
      else {
        const timer = setTimeout(resolve, 1000);
        pc.addEventListener("icegatheringstatechange", () => {
          if (pc.iceGatheringState === "complete") {
            clearTimeout(timer);
            resolve();
          }
        });
      }
    });
    const response = await fetch(whep, {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: pc.localDescription.sdp,
    });
    if (!response.ok) throw new Error(String(response.status));
    await pc.setRemoteDescription({ type: "answer", sdp: await response.text() });
    const timer = setTimeout(() => {
      if (video.readyState < 2) {
        pc.close();
        startHls(channel.index, video, hlsUrl);
      }
    }, 4000);
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === "failed" || pc.connectionState === "disconnected") {
        clearTimeout(timer);
        pc.close();
        startHls(channel.index, video, hlsUrl);
      }
      if (pc.connectionState === "connected") clearTimeout(timer);
    };
    players.set(channel.index, { pc, video });
    await video.play().catch(() => {});
  } catch {
    startHls(channel.index, video, hlsUrl);
  }
}

function startHls(index, video, url) {
  const existing = players.get(index);
  if (existing && existing.hls) return;
  if (video.canPlayType("application/vnd.apple.mpegurl")) {
    video.src = url;
    video.play().catch(() => {});
    players.set(index, { video });
    return;
  }
  const hls = new Hls({ lowLatencyMode: true, enableWorker: true });
  hls.loadSource(url);
  hls.attachMedia(video);
  hls.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
  players.set(index, { ...(existing || {}), hls, video });
}

function stopTile(index) {
  const player = players.get(index);
  if (!player) return;
  if (player.pc) player.pc.close();
  if (player.hls) player.hls.destroy();
  players.delete(index);
}

function toggleMute(channel, event) {
  event.stopPropagation();
  const video = event.target.closest(".tile").querySelector("video");
  if (video) video.muted = !video.muted;
}

function onVideo(channel, el) {
  if (el) playTile(channel, el);
}

async function saveChannel(channel) {
  message.value = "";
  const response = await fetch(`/api/v1/channels/${channel.index}`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      video_label: channel.video_label,
      audio_label: channel.audio_label,
      preview_height: Number(channel.preview_height),
      video_bitrate_kbps: Number(channel.video_bitrate_kbps),
      audio_bitrate_kbps: Number(channel.audio_bitrate_kbps),
      max_fps: Number(channel.max_fps),
      audio_pair: Number(channel.audio_pair),
      downmix: channel.downmix,
      overlay: channel.overlay,
    }),
  });
  messageOk.value = response.ok;
  message.value = response.ok ? `Channel ${channel.index} updated` : await response.text();
}

async function saveConfig() {
  message.value = "";
  let body = configText.value;
  try {
    body = JSON.stringify(JSON.parse(configText.value));
  } catch {
    messageOk.value = false;
    message.value = "Config text is not JSON";
    return;
  }
  const response = await fetch("/api/v1/config", { method: "PUT", headers: { "Content-Type": "application/json" }, body });
  messageOk.value = response.ok;
  message.value = response.ok ? "Config saved" : await response.text();
  if (response.ok) await load();
}

let socket;
onMounted(async () => {
  await load();
  socket = connectEvents();
});
onBeforeUnmount(() => {
  if (socket) socket.close();
  for (const index of players.keys()) stopTile(index);
});
</script>

<template>
  <header>
    <h1>MXL WebRTC Monitor</h1>
    <div class="sub" v-if="info">{{ info.version }} · {{ info.encoder_available }} · MediaMTX {{ info.mediamtx }}</div>
  </header>
  <div class="banner" v-if="config && config.restart_required">A global setting changed. Restart the process to apply it.</div>
  <nav>
    <button :class="{ active: tab === 'multiview' }" @click="tab = 'multiview'">Multiview</button>
    <button :class="{ active: tab === 'channels' }" @click="tab = 'channels'">Channels</button>
    <button :class="{ active: tab === 'nmos' }" @click="tab = 'nmos'">NMOS</button>
    <button :class="{ active: tab === 'settings' }" @click="tab = 'settings'">Settings</button>
  </nav>
  <main>
    <section v-show="tab === 'multiview'">
      <div class="toolbar">
        <label>Layout
          <select v-model="layout">
            <option value="auto">Automatic</option>
            <option value="1">1×1</option>
            <option value="2">2×2</option>
            <option value="3">3×3</option>
            <option value="4">4×4</option>
          </select>
        </label>
        <button class="btn secondary" v-if="expanded" @click="expanded = 0">Show all</button>
      </div>
      <div class="grid" :style="{ gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))` }">
        <article
          v-for="channel in channels"
          :key="channel.index"
          class="tile"
          :class="{ expanded: expanded === channel.index, hidden: expanded && expanded !== channel.index }"
          @click="expanded = expanded === channel.index ? 0 : channel.index"
        >
          <video :ref="(el) => onVideo(channel, el)" autoplay playsinline muted></video>
          <div class="meta">
            <div>
              <div class="name">{{ channel.video_label }}</div>
              <div class="src">{{ channel.source_label || "no source" }} · {{ channel.format || "—" }} · {{ channel.encoder || "—" }}</div>
            </div>
            <div>
              <span class="badge" :class="channel.video.state">{{ channel.video.state }}</span>
              <button class="btn secondary" @click="toggleMute(channel, $event)">Mute</button>
              <div class="meters">
                <div
                  v-for="(peak, idx) in channel.meters.peak_dbfs"
                  :key="idx"
                  class="meter"
                  :class="{ selected: selectedPair(channel).includes(idx + 1), hot: peak > -6 }"
                >
                  <i :style="{ height: meterHeight(peak) }"></i>
                </div>
              </div>
            </div>
          </div>
        </article>
      </div>
    </section>

    <section v-show="tab === 'channels'">
      <p class="sub">Routing is IS-05 only. These settings apply without a restart.</p>
      <article class="panel" v-for="channel in channels" :key="channel.index">
        <h2>Channel {{ channel.index }}</h2>
        <div class="fields">
          <div><label>Video label</label><input v-model="channel.video_label" /></div>
          <div><label>Audio label</label><input v-model="channel.audio_label" /></div>
          <div><label>Preview height</label><input type="number" v-model.number="channel.preview_height" /></div>
          <div><label>Video kbit/s</label><input type="number" v-model.number="channel.video_bitrate_kbps" /></div>
          <div><label>Audio kbit/s</label><input type="number" v-model.number="channel.audio_bitrate_kbps" /></div>
          <div><label>Max fps (0 = source)</label><input type="number" v-model.number="channel.max_fps" /></div>
          <div><label>Audio pair</label><input type="number" v-model.number="channel.audio_pair" /></div>
          <div><label>Downmix</label>
            <select v-model="channel.downmix"><option>stereo</option><option>mono</option></select>
          </div>
          <div><label>Overlay</label><input type="checkbox" v-model="channel.overlay" /></div>
        </div>
        <div class="row" style="margin-top:.6rem">
          <button class="btn" @click="saveChannel(channel)">Apply</button>
          <span class="badge" :class="channel.video.state">video {{ channel.video.state }}</span>
          <span class="badge" :class="channel.audio.state">audio {{ channel.audio.state }}</span>
        </div>
      </article>
      <p class="msg" :class="messageOk ? 'ok' : 'err'">{{ message }}</p>
    </section>

    <section v-show="tab === 'nmos'" v-if="nmos">
      <article class="panel">
        <h2>Node</h2>
        <p>{{ nmos.device_label }} · {{ nmos.node_id }}</p>
        <p>Registry {{ nmos.registry || "none" }}:{{ nmos.registry_port }} · DNS-SD {{ nmos.dns_sd }} · registered {{ nmos.registered }}</p>
      </article>
      <article class="panel">
        <h2>Receivers</h2>
        <table>
          <thead><tr><th>Label</th><th>Kind</th><th>State</th><th>Sender</th><th>Domain</th><th>Flow</th></tr></thead>
          <tbody>
            <tr v-for="receiver in nmos.receivers" :key="receiver.id">
              <td>{{ receiver.label }}<div class="sub">{{ receiver.id }}</div></td>
              <td>{{ receiver.kind }}</td>
              <td><span class="badge" :class="receiver.state">{{ receiver.state }}</span></td>
              <td>{{ receiver.sender_id || "—" }}</td>
              <td>{{ receiver.mxl_domain_id || "—" }}</td>
              <td>{{ receiver.mxl_flow_id || "—" }}</td>
            </tr>
          </tbody>
        </table>
      </article>
    </section>

    <section v-show="tab === 'settings'" v-if="config">
      <article class="panel">
        <h2>Effective configuration</h2>
        <table>
          <thead><tr><th>Key</th><th>Value</th><th>Origin</th></tr></thead>
          <tbody>
            <tr v-for="(value, key) in config.values" :key="key">
              <td>{{ key }}</td>
              <td>{{ value }}</td>
              <td>{{ config.origin[key] || "default" }}</td>
            </tr>
          </tbody>
        </table>
      </article>
      <article class="panel">
        <h2>Import / export</h2>
        <p class="sub">Environment keys stay read-only. Global keys need a restart. Channel keys apply immediately after the file is saved.</p>
        <textarea v-model="configText"></textarea>
        <div class="row" style="margin-top:.6rem">
          <button class="btn" @click="saveConfig">Save JSON</button>
          <button class="btn secondary" @click="navigator.clipboard.writeText(envText)">Copy KEY=value</button>
        </div>
        <textarea readonly v-model="envText" style="margin-top:.6rem"></textarea>
        <p class="msg" :class="messageOk ? 'ok' : 'err'">{{ message }}</p>
      </article>
    </section>
  </main>
</template>
