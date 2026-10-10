<script setup>
// Settings (§6.2, §7): every setting with its value and origin, edits saved to the configuration
// file (PUT /api/v1/config), export (JSON document and KEY=value) and import.
import { computed, onMounted, ref } from "vue";
import OriginBadge from "./OriginBadge.vue";
import { api, copyText, download } from "../api.js";
import { hasConfigFile, importConfig, importDraft, live, refreshConfig, saveSettings, settingsEdits } from "../store.js";

const exportDoc = ref("");
const envText = ref("");
const exportMsg = ref("");
const importMsg = ref({ kind: "", text: "" });

const CHANNEL_KEY = /^CH(\d+)_/;
const GROUPS = [
  { title: "Channels and encoding", test: /^(MONITOR_(CHANNELS|PREVIEW_HEIGHT|MAX_FPS|VIDEO_BITRATE_KBPS|AUDIO_BITRATE_KBPS)|READ_OFFSET_GRAINS|ENCODER)$/ },
  { title: "Previews, MediaMTX and widgets", test: /^(MONITOR_PUBLIC_IP|PREVIEW_|MEDIAMTX_|WIDGET_)/ },
  { title: "NMOS", test: /^(NMOS_|HOST_ID)/ },
  { title: "Tally (TSL)", test: /^TSL_/ },
  { title: "MXL, files and process", test: /./ },
];
// One line per key, from the README settings table.
const DESCRIPTIONS = {
  HOST_ID: "Seed and label fallback. Not an announced address.",
  MXL_DOMAIN_SCAN_PATH: "Parent of the input domains, including mirror-*.",
  MONITOR_CHANNELS: "Number of channels (1–16).",
  MONITOR_PREVIEW_HEIGHT: "Default preview height of a channel.",
  MONITOR_MAX_FPS: "Default frame-rate limit (0 = source rate).",
  MONITOR_VIDEO_BITRATE_KBPS: "Default H.264 bitrate per channel.",
  MONITOR_AUDIO_BITRATE_KBPS: "Default Opus bitrate per channel.",
  READ_OFFSET_GRAINS: "Grains read behind the flow's head.",
  ENCODER: "auto (NVENC when it works, else x264), nvenc or x264.",
  MONITOR_PUBLIC_IP: "ICE address of WebRTC; default for NMOS_HOST_ADDRESS.",
  NMOS_HOST_ADDRESS: "IP on the IS-04 href, API endpoints and IS-05 control hrefs.",
  PREVIEW_PUBLISH_URL: "RTSP of a shared MediaMTX (empty: the built-in one). Was MEDIAMTX_RTSP_URL.",
  PREVIEW_PATH_PREFIX: "Path prefix of the streams: <prefix>/ch<n>.",
  PREVIEW_WHEP_URL: "Browser WHEP base (empty: the page's host and MEDIAMTX_WHEP_PORT). Was MONITOR_WHEP_PUBLIC_URL.",
  PREVIEW_HLS_URL: "Browser HLS base (empty: the page's host and MEDIAMTX_HLS_PORT). Was MONITOR_HLS_PUBLIC_URL.",
  WIDGET_FRAME_ANCESTORS: "Who may frame /widget pages (CSP frame-ancestors).",
  STATE_DIR: "is05.json and, by default, mediamtx.yml.",
  SHUTDOWN_TIMEOUT_S: "Seconds allowed after SIGTERM.",
  MXL_CLEANUP_ON_EXIT: "Accepted; the monitor has no output domain to remove.",
  NMOS_LABEL: "Node label and device label prefix.",
  NMOS_TAGS: "Tags on the node and device (JSON object of string lists).",
  MEDIAMTX_RTSP_PORT: "RTSP ingest of the built-in MediaMTX (localhost).",
  MEDIAMTX_API_URL: "MediaMTX API: viewers and stream state (shared mode: only when set).",
  MEDIAMTX_CONFIG_PATH: "Generated MediaMTX configuration.",
  MEDIAMTX_WHEP_PORT: "WHEP port of the built-in MediaMTX.",
  MEDIAMTX_HLS_PORT: "HLS port of the built-in MediaMTX.",
  MEDIAMTX_ICE_UDP_PORT: "ICE UDP and TCP port.",
  MEDIAMTX_METRICS_PORT: "MediaMTX metrics (0: API port + 1).",
  NMOS_ENABLE: "Run the NMOS node.",
  NMOS_REGISTRY_ADDRESS: "Registration API host (empty: no registration).",
  NMOS_REGISTRY_PORT: "Registration API port.",
  NMOS_QUERY_ADDRESS: "Query API host (default: the registry).",
  NMOS_QUERY_PORT: "Query API port (default: registration port + 1).",
  NMOS_DNS_SD: "DNS-SD browsing and mDNS advertisement.",
  NMOS_PORT: "Node and connection API; the WebSocket is the next port.",
  NMOS_SEED: "UUIDv5 seed of the node, device and receivers.",
  WEB_PORT: "This UI, the API, health and metrics.",
  LOG_LEVEL: "Log level.",
  METRICS_AUDIO_PEAK: "Publish per-input peak gauges.",
  TSL_ENABLE: "Listen for TSL UMD 5.0 tally.",
  TSL_UDP_PORT: "TSL UDP port.",
  TSL_TCP_PORT: "TSL TCP port (DLE/STX framing).",
  TSL_SCREEN: "Only this TSL screen (-1: every screen).",
  TSL_MAP: "TSL display to channel, e.g. 0:1,1:2 (empty: display i is channel i+1).",
  MONITOR_CONFIG_FILE: "This configuration file (environment only).",
};

const settings = computed(() => {
  const values = live.config?.values || {};
  const origin = live.config?.origin || {};
  return Object.keys(values)
    .sort()
    .map((key) => ({
      key,
      value: values[key],
      source: origin[key] || "default",
      editable: hasConfigFile.value && origin[key] !== "env" && key !== "MONITOR_CONFIG_FILE",
    }));
});
const groups = computed(() => {
  const left = settings.value.filter((s) => !CHANNEL_KEY.test(s.key));
  return GROUPS.map((g) => {
    const items = left.filter((s) => g.test.test(s.key));
    items.forEach((s) => left.splice(left.indexOf(s), 1));
    return { title: g.title, items };
  }).filter((g) => g.items.length);
});
// Channel keys are edited on the Channels tab; here only the ones the environment or the file set.
const channelOverrides = computed(() => settings.value.filter((s) => CHANNEL_KEY.test(s.key) && s.source !== "default"));
const changed = computed(() => Object.keys(settingsEdits));

const value = (s) => (s.key in settingsEdits ? (settingsEdits[s.key] ?? "") : s.value);
function edit(s, v) {
  if (v === s.value) delete settingsEdits[s.key];
  else settingsEdits[s.key] = v;
}
const reset = (s) => (settingsEdits[s.key] = null);
const discard = () => changed.value.forEach((k) => delete settingsEdits[k]);

async function loadExport() {
  try {
    exportDoc.value = JSON.stringify(await api.get("/api/v1/config/export"), null, 2);
    envText.value = await api.text("/api/v1/config.env");
  } catch (e) {
    exportMsg.value = e.message;
  }
}
onMounted(() => {
  refreshConfig();
  loadExport();
});

async function save() {
  if (await saveSettings()) loadExport();
}
async function copy(text, what) {
  exportMsg.value = (await copyText(text)) ? `${what} copied.` : "Copy failed; select the text and copy it by hand.";
}
function onFile(ev) {
  ev.target.files?.[0]?.text().then((t) => (importDraft.text = t));
}
async function doImport() {
  try {
    JSON.parse(importDraft.text);
  } catch (e) {
    importMsg.value = { kind: "err", text: `Not JSON: ${e.message}` };
    return;
  }
  try {
    await importConfig(importDraft.text);
    importMsg.value = {
      kind: "ok",
      text: "Imported into the configuration file. Channel settings apply now; other settings after a restart. Keys set by the environment were skipped.",
    };
    importDraft.text = "";
    loadExport();
  } catch (e) {
    importMsg.value = { kind: "err", text: e.message };
  }
}
const seed = computed(() => live.config?.values?.NMOS_SEED || "monitor");
</script>

<template>
  <div class="panel">
    <h3>Configuration</h3>
    <p class="note" style="margin-top: 0">
      Precedence: environment (ENV), then the configuration file (FILE), then the default. ENV settings are read-only here. Saved values go into
      <code>{{ live.config?.values?.MONITOR_CONFIG_FILE || "MONITOR_CONFIG_FILE" }}</code>. The settings on this page apply when the monitor starts
      again; the channel settings (Channels tab) apply at once.
    </p>
    <div v-if="live.config && !hasConfigFile" class="warnbox">
      MONITOR_CONFIG_FILE is not set: settings cannot be saved or imported here. Channel changes last until the monitor restarts.
    </div>
    <div class="actions" style="margin-top: 0">
      <span class="spacer"></span>
      <button class="btn secondary" :disabled="!changed.length" @click="discard">Discard</button>
      <button class="btn" :disabled="!changed.length" @click="save">Save{{ changed.length ? ` (${changed.length})` : "" }}</button>
    </div>
  </div>

  <div v-for="g in groups" :key="g.title" class="panel">
    <h3>{{ g.title }}</h3>
    <table>
      <thead>
        <tr><th style="width: 34%">Key</th><th>Value</th><th style="width: 6rem">Origin</th><th style="width: 5.5rem"></th></tr>
      </thead>
      <tbody>
        <tr v-for="s in g.items" :key="s.key">
          <td>
            <code>{{ s.key }}</code>
            <div class="desc">{{ DESCRIPTIONS[s.key] || "" }}</div>
          </td>
          <td>
            <input
              :value="value(s)"
              :disabled="!s.editable"
              :class="{ dirty: s.key in settingsEdits }"
              :aria-label="s.key"
              :placeholder="settingsEdits[s.key] === null ? '(default)' : ''"
              @input="edit(s, $event.target.value)"
            />
          </td>
          <td><OriginBadge :origin="s.source" all /></td>
          <td>
            <button v-if="s.source === 'file' && s.editable" class="btn small secondary" title="Remove the file value" @click="reset(s)">Default</button>
          </td>
        </tr>
      </tbody>
    </table>
  </div>

  <div v-if="channelOverrides.length" class="panel">
    <h3>Channel settings from the environment or the file</h3>
    <table>
      <thead>
        <tr><th style="width: 34%">Key</th><th>Value</th><th style="width: 6rem">Origin</th></tr>
      </thead>
      <tbody>
        <tr v-for="s in channelOverrides" :key="s.key">
          <td><code>{{ s.key }}</code></td>
          <td style="overflow-wrap: anywhere">{{ s.value }}</td>
          <td><OriginBadge :origin="s.source" all /></td>
        </tr>
      </tbody>
    </table>
    <p class="note">Change these on the Channels tab (CH&lt;n&gt;_* keys; ENV ones are read-only).</p>
  </div>

  <div class="grid two">
    <div class="panel">
      <h3>Export</h3>
      <p class="note">One JSON document with every setting, including the channel keys. The monitor has no secrets.</p>
      <div class="actions" style="margin-top: 0">
        <span class="msg ok" style="margin: 0">{{ exportMsg }}</span>
        <span class="spacer"></span>
        <button class="btn secondary" :disabled="!exportDoc" @click="copy(exportDoc, 'JSON')">Copy JSON</button>
        <button class="btn" :disabled="!exportDoc" @click="download(`mxl-webrtc-monitor-${seed}.json`, exportDoc + '\n')">Download JSON</button>
      </div>
      <pre style="max-height: 320px">{{ exportDoc }}</pre>
      <div class="actions">
        <button class="btn secondary" :disabled="!envText" @click="copy(envText, 'KEY=value list')">Copy KEY=value</button>
        <button class="btn secondary" :disabled="!envText" @click="download(`mxl-webrtc-monitor-${seed}.env`, envText, 'text/plain')">Download KEY=value</button>
      </div>
    </div>
    <div class="panel">
      <h3>Import</h3>
      <p class="note">An exported document. It replaces the configuration file; keys set by the environment are skipped.</p>
      <input type="file" accept="application/json,.json" aria-label="Open an exported JSON file" :disabled="!hasConfigFile" @change="onFile" />
      <textarea
        v-model="importDraft.text"
        aria-label="Exported JSON"
        placeholder='{"version": 1, "settings": {...}}'
        style="margin-top: 0.5rem"
        :disabled="!hasConfigFile"
      ></textarea>
      <div class="actions">
        <span class="msg" :class="importMsg.kind" style="margin: 0">{{ importMsg.text }}</span>
        <span class="spacer"></span>
        <button class="btn" :disabled="!hasConfigFile || !importDraft.text.trim()" @click="doImport">Import</button>
      </div>
    </div>
  </div>
</template>
