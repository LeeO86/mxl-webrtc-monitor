<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
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
const drafts = ref({});
// Per channel index: the tile's <video> and its player ({ key, video, pc, hls, timer }).
const videoEls = new Map();
const players = new Map();
const editable = ["video_label", "audio_label", "preview_height", "video_bitrate_kbps", "audio_bitrate_kbps", "max_fps", "audio_pair", "downmix", "overlay"];

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

function resolvePlayback(channel) {
  const flags = channel.playback?.public || {};
  const whep = flags.whep ? channel.playback.whep : pageUrl(channel.playback.whep);
  const hls = flags.hls ? channel.playback.hls : pageUrl(channel.playback.hls);
  const blocked =
    location.protocol === "https:" &&
    (String(whep).startsWith("http:") || String(hls).startsWith("http:"));
  return { whep, hls, blocked };
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

// The server pushes the status about 10 times a second. Merging it into the existing channel
// objects keeps the tiles (and their players) in place.
function applyStatus(payload) {
  if (!payload || !payload.channels) return;
  const byIndex = new Map(channels.value.map((channel) => [channel.index, channel]));
  const next = payload.channels.map((incoming) => {
    const existing = byIndex.get(incoming.index);
    if (!existing) return incoming;
    Object.assign(existing, incoming);
    return existing;
  });
  if (next.length !== channels.value.length || next.some((channel, i) => channel !== channels.value[i])) channels.value = next;
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

// A player restarts only when what it plays changes: the URLs, the video state or flow, or whether
// audio is routed (the monitor rebuilds the stream with or without an audio track).
function playbackKey(channel) {
  const resolved = resolvePlayback(channel);
  const audioRouted = Boolean(channel.audio?.master_enable && channel.audio?.mxl_flow_id);
  return JSON.stringify([resolved.whep, resolved.hls, channel.video?.state, channel.video?.mxl_flow_id || "", audioRouted]);
}

function syncPlayers() {
  const present = new Set();
  for (const channel of channels.value) {
    present.add(channel.index);
    const video = videoEls.get(channel.index);
    if (!video || !video.isConnected) continue;
    const key = playbackKey(channel);
    const player = players.get(channel.index);
    if (player && player.key === key && player.video === video) continue;
    playTile(channel, video, key);
  }
  for (const index of [...players.keys()]) {
    if (!present.has(index)) {
      stopTile(index);
      videoEls.delete(index);
    }
  }
}

// Vue calls a function ref on every render of the tile: only a new element counts.
function setVideoRef(index, el) {
  if (!el || videoEls.get(index) === el) return;
  videoEls.set(index, el);
  syncPlayers();
}

async function playTile(channel, video, key) {
  stopTile(channel.index);
  const entry = { key, video };
  players.set(channel.index, entry);
  const current = () => players.get(channel.index) === entry;
  video.muted = true;
  const resolved = resolvePlayback(channel);
  if (resolved.blocked && resolved.whep.startsWith("http:") && resolved.hls.startsWith("http:")) {
    return;
  }
  const whep = resolved.whep;
  const hlsUrl = resolved.hls;
  try {
    const pc = new RTCPeerConnection();
    entry.pc = pc;
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });
    const stream = new MediaStream();
    pc.ontrack = (ev) => {
      stream.addTrack(ev.track);
      if (current()) video.srcObject = stream;
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
    if (!current()) return;
    const response = await fetch(whep, {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: pc.localDescription.sdp,
    });
    if (!response.ok) throw new Error(String(response.status));
    const answer = await response.text();
    if (!current()) return;
    await pc.setRemoteDescription({ type: "answer", sdp: answer });
    entry.timer = setTimeout(() => {
      if (current() && video.readyState < 2) startHls(entry, hlsUrl);
    }, 4000);
    pc.onconnectionstatechange = () => {
      if (!current()) return;
      if (pc.connectionState === "failed" || pc.connectionState === "disconnected") startHls(entry, hlsUrl);
      if (pc.connectionState === "connected") clearTimeout(entry.timer);
    };
    await video.play().catch(() => {});
  } catch {
    if (current()) startHls(entry, hlsUrl);
  }
}

// Falls back from WHEP to HLS: the peer connection's stream must leave the element first.
function startHls(entry, url) {
  if (entry.hls || entry.nativeHls) return;
  clearTimeout(entry.timer);
  if (entry.pc) {
    entry.pc.close();
    entry.pc = null;
  }
  const video = entry.video;
  video.srcObject = null;
  if (video.canPlayType("application/vnd.apple.mpegurl")) {
    entry.nativeHls = true;
    video.src = url;
    video.play().catch(() => {});
    return;
  }
  const hls = new Hls({ lowLatencyMode: true, enableWorker: true });
  entry.hls = hls;
  hls.loadSource(url);
  hls.attachMedia(video);
  hls.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
}

function stopTile(index) {
  const player = players.get(index);
  if (!player) return;
  clearTimeout(player.timer);
  if (player.pc) player.pc.close();
  if (player.hls) player.hls.destroy();
  if (player.video) {
    player.video.srcObject = null;
    if (player.nativeHls) player.video.removeAttribute("src");
  }
  players.delete(index);
}

function toggleMute(channel, event) {
  event.stopPropagation();
  const video = event.target.closest(".tile").querySelector("video");
  if (video) video.muted = !video.muted;
}

// The Channels tab edits a copy, so the status pushes do not overwrite what is being typed.
function syncDrafts() {
  for (const channel of channels.value) {
    if (!drafts.value[channel.index]) {
      drafts.value[channel.index] = Object.fromEntries(editable.map((key) => [key, channel[key]]));
    }
  }
}

function copyEnv() {
  navigator.clipboard?.writeText(envText.value);
}

async function saveChannel(channel) {
  message.value = "";
  const draft = drafts.value[channel.index];
  const response = await fetch(`/api/v1/channels/${channel.index}`, {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      video_label: draft.video_label,
      audio_label: draft.audio_label,
      preview_height: Number(draft.preview_height),
      video_bitrate_kbps: Number(draft.video_bitrate_kbps),
      audio_bitrate_kbps: Number(draft.audio_bitrate_kbps),
      max_fps: Number(draft.max_fps),
      audio_pair: Number(draft.audio_pair),
      downmix: draft.downmix,
      overlay: draft.overlay,
    }),
  });
  messageOk.value = response.ok;
  message.value = response.ok ? `Channel ${channel.index} updated` : await response.text();
  if (response.ok) {
    delete drafts.value[channel.index];
    syncDrafts();
  }
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

watch(
  channels,
  () => {
    syncPlayers();
    syncDrafts();
  },
  { flush: "post" },
);

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
          <video :ref="(el) => setVideoRef(channel.index, el)" autoplay playsinline muted></video>
          <div class="meta">
            <div>
              <div class="name">{{ channel.video_label }}</div>
              <div class="src">{{ channel.source_label || "no source" }} · {{ channel.format || "—" }} · {{ channel.encoder || "—" }}</div>
              <div class="hint" v-if="resolvePlayback(channel).blocked">playback blocked: set MONITOR_WHEP_PUBLIC_URL / MONITOR_HLS_PUBLIC_URL</div>
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
        <div class="fields" v-if="drafts[channel.index]">
          <div><label>Video label</label><input v-model="drafts[channel.index].video_label" /></div>
          <div><label>Audio label</label><input v-model="drafts[channel.index].audio_label" /></div>
          <div><label>Preview height</label><input type="number" v-model.number="drafts[channel.index].preview_height" /></div>
          <div><label>Video kbit/s</label><input type="number" v-model.number="drafts[channel.index].video_bitrate_kbps" /></div>
          <div><label>Audio kbit/s</label><input type="number" v-model.number="drafts[channel.index].audio_bitrate_kbps" /></div>
          <div><label>Max fps (0 = source)</label><input type="number" v-model.number="drafts[channel.index].max_fps" /></div>
          <div><label>Audio pair</label><input type="number" v-model.number="drafts[channel.index].audio_pair" /></div>
          <div><label>Downmix</label>
            <select v-model="drafts[channel.index].downmix"><option>stereo</option><option>mono</option></select>
          </div>
          <div><label>Overlay</label><input type="checkbox" v-model="drafts[channel.index].overlay" /></div>
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
          <button class="btn secondary" @click="copyEnv">Copy KEY=value</button>
        </div>
        <textarea readonly v-model="envText" style="margin-top:.6rem"></textarea>
        <p class="msg" :class="messageOk ? 'ok' : 'err'">{{ message }}</p>
      </article>
    </section>
  </main>
</template>
