// Shared UI state: info, channels (WebSocket /api/v1/events, ten times a second), NMOS, the
// configuration and readiness (polled), the drafts that survive tab switches and reconnects, and
// the actions every page uses.
import { computed, reactive } from "vue";
import { api, canon, clone, probe } from "./api.js";

function stored(key, fallback) {
  try {
    return localStorage.getItem(`mxl-webrtc-monitor.${key}`) ?? fallback;
  } catch {
    return fallback;
  }
}
function store(key, value) {
  try {
    localStorage.setItem(`mxl-webrtc-monitor.${key}`, String(value));
  } catch {
    /* kept for this tab only */
  }
}

export const live = reactive({
  info: null, // GET /api/v1/info
  channels: [], // channel status, newest from the WebSocket
  nmos: null, // GET /api/v1/nmos
  config: null, // GET /api/v1/config: values, origin, restart_required, channels
  ready: null, // GET /readyz: { status, body: { mxl_root, nmos, mediamtx } }
  connected: false,
  everConnected: false,
  error: "", // API unreachable
  actionError: "", // the last failed action (banner)
  notice: "", // the last action's result worth showing (banner)
  selected: Number(stored("channel", 1)) || 1, // the channel the Channels tab edits
});

// Multiview layout (columns, "auto") and the tile shown full size; kept in this browser.
export const view = reactive({ layout: stored("layout", "auto"), expanded: 0 });
export function setLayout(value) {
  view.layout = value;
  store("layout", value);
}

export const selectedChannel = computed(() => live.channels.find((c) => c.index === live.selected) || live.channels[0] || null);
export function selectChannel(index) {
  live.selected = index;
  store("channel", index);
}

/** The configuration file is set: settings and imports can be saved (PUT/POST answer 409 otherwise). */
export const hasConfigFile = computed(() => Boolean(live.config?.values?.MONITOR_CONFIG_FILE));
/** Origin of a setting: "env", "file" or "default". */
export const originOf = (key) => live.config?.origin?.[key] || "default";
/** The NMOS receiver of a channel's video or audio (GET /api/v1/nmos lists them per channel, video first). */
export function receiverOf(index, kind) {
  const rx = live.nmos?.receivers?.[(index - 1) * 2 + (kind === "audio" ? 1 : 0)];
  return rx && rx.kind === kind ? rx : null;
}

/** Runs an action; a failure goes to the error banner. Returns the result (true for an empty answer), or undefined. */
export async function act(fn, notice = "") {
  try {
    const result = await fn();
    live.actionError = "";
    if (notice) live.notice = typeof notice === "function" ? notice(result) : notice;
    return result ?? true;
  } catch (e) {
    live.actionError = e.message;
    return undefined;
  }
}

// ---- channel settings drafts -------------------------------------------------
// A draft is taken from the status while it is unchanged and kept while it is edited, so tab
// switches, WebSocket reconnects and status pushes never overwrite an edit.

export const CHANNEL_FIELDS = [
  "video_label",
  "audio_label",
  "preview_height",
  "max_fps",
  "video_bitrate_kbps",
  "audio_pair",
  "downmix",
  "audio_bitrate_kbps",
  "overlay",
  "overlay_label",
  "overlay_source",
  "overlay_format",
];
export const settingsOf = (channel) => Object.fromEntries(CHANNEL_FIELDS.map((k) => [k, channel?.[k]]));

export const drafts = reactive({}); // channel index -> { value, base }

function syncDraft(index, fresh) {
  const d = drafts[index];
  const c = canon(fresh);
  if (!d || canon(d.value) === d.base) {
    if (!d || d.base !== c) drafts[index] = { value: clone(fresh), base: c };
  } else {
    d.base = c; // edited: the draft stays, compared with the newest status
  }
}
export const isDirty = (index) => {
  const d = drafts[index];
  return !!d && canon(d.value) !== d.base;
};
export function revertDraft(index) {
  const channel = live.channels.find((c) => c.index === index);
  if (channel) drafts[index] = { value: settingsOf(channel), base: canon(settingsOf(channel)) };
}

/** PATCH /api/v1/channels/{n} with the changed fields of the draft; the answer is every channel's status. */
export async function applyDraft(index) {
  const d = drafts[index];
  const channel = live.channels.find((c) => c.index === index);
  if (!d || !channel) return undefined;
  const base = settingsOf(channel);
  const body = Object.fromEntries(Object.entries(d.value).filter(([k, v]) => v !== base[k]));
  const result = await act(() => api.patch(`/api/v1/channels/${index}`, body), `Channel ${index} updated.`);
  if (result) {
    applyStatus(result);
    revertDraft(index);
    refreshConfig();
  }
  return result;
}

// ---- settings (configuration file) -------------------------------------------

export const settingsEdits = reactive({}); // key -> new value, or null to remove the file value
export const importDraft = reactive({ text: "" });

/** PUT /api/v1/config: the file layer is replaced, so it carries every file value plus the edits. */
export async function saveSettings() {
  const values = live.config?.values || {};
  const body = {};
  for (const [key, origin] of Object.entries(live.config?.origin || {})) {
    if (origin === "file") body[key] = values[key];
  }
  for (const [key, value] of Object.entries(settingsEdits)) {
    if (value === null) delete body[key];
    else body[key] = value;
  }
  const keys = Object.keys(settingsEdits);
  const result = await act(() => api.put("/api/v1/config", body));
  if (result) {
    live.config = result;
    for (const key of keys) delete settingsEdits[key];
    live.notice = `Saved ${keys.join(", ")} to the configuration file.`;
    refreshChannels();
  }
  return result;
}

export async function importConfig(text) {
  const result = await api.post("/api/v1/config/import", text);
  live.config = result;
  refreshChannels();
  return result;
}

// ---- live connection ---------------------------------------------------------

// The server pushes the status about 10 times a second. Merging it into the existing channel
// objects keeps the tiles (and their players) in place.
function applyStatus(payload) {
  if (!payload || !payload.channels) return;
  const byIndex = new Map(live.channels.map((channel) => [channel.index, channel]));
  const next = payload.channels.map((incoming) => {
    const existing = byIndex.get(incoming.index);
    if (!existing) return incoming;
    Object.assign(existing, incoming);
    return existing;
  });
  if (next.length !== live.channels.length || next.some((channel, i) => channel !== live.channels[i])) live.channels = next;
  for (const channel of live.channels) syncDraft(channel.index, settingsOf(channel));
}

async function refreshChannels() {
  try {
    applyStatus(await api.get("/api/v1/channels"));
    if (!live.info) live.info = await api.get("/api/v1/info");
    live.error = "";
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
}

export async function refreshConfig() {
  try {
    live.config = await api.get("/api/v1/config");
  } catch {
    /* keep the last answer; the connection banner tells */
  }
}

async function refreshSlow() {
  live.ready = await probe("/readyz");
  try {
    live.nmos = await api.get("/api/v1/nmos");
  } catch {
    /* keep the last answer */
  }
  refreshConfig();
}

let socket = null;
let retry = 0;
let timers = [];

function connect() {
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  socket = new WebSocket(`${proto}//${location.host}/api/v1/events`);
  socket.onopen = () => {
    live.connected = true;
    live.everConnected = true;
    live.error = "";
  };
  socket.onmessage = (ev) => {
    try {
      applyStatus(JSON.parse(ev.data));
    } catch {
      /* a broken frame is dropped; the next one replaces it */
    }
  };
  socket.onclose = () => {
    live.connected = false;
    retry = setTimeout(connect, 1000);
  };
}

/** Loads info, channels, NMOS, configuration and readiness, then follows the WebSocket; polls while it is down. */
export async function startLive() {
  try {
    live.info = await api.get("/api/v1/info");
  } catch (e) {
    live.error = `API unreachable: ${e.message}`;
  }
  await refreshChannels();
  refreshSlow();
  connect();
  timers = [
    setInterval(() => {
      if (!live.connected) refreshChannels();
    }, 2000),
    // Registration, readiness and the configuration change without a push.
    setInterval(refreshSlow, 5000),
  ];
}

export function stopLive() {
  timers.forEach(clearInterval);
  clearTimeout(retry);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
}
