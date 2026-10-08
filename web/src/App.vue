<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./components/Pill.vue";
import MultiviewTab from "./components/MultiviewTab.vue";
import ChannelsTab from "./components/ChannelsTab.vue";
import NmosTab from "./components/NmosTab.vue";
import StatusTab from "./components/StatusTab.vue";
import SettingsTab from "./components/SettingsTab.vue";
import { resolvePlayback } from "./player.js";
import { isDirty, live, settingsEdits, startLive, stopLive } from "./store.js";

// The multiview stays mounted (v-show): switching tabs does not stop or restart its players.
const tabs = [
  { id: "multiview", label: "Multiview", full: true },
  { id: "channels", label: "Channels", component: ChannelsTab },
  { id: "nmos", label: "NMOS", component: NmosTab },
  { id: "status", label: "Status", component: StatusTab },
  { id: "settings", label: "Settings", component: SettingsTab },
];

const current = ref("multiview");
const tab = computed(() => tabs.find((t) => t.id === current.value));

function onHash() {
  const id = location.hash.slice(1);
  if (tabs.some((t) => t.id === id)) current.value = id;
}
function go(id) {
  current.value = id;
  location.hash = id;
}

const count = (pred) => live.channels.filter(pred).length;
const running = computed(() => count((c) => c.video.state === "running"));
const waiting = computed(() => count((c) => c.video.state === "waiting" || c.audio.state === "waiting"));
const noSignal = computed(() => count((c) => c.video.state === "no_signal" || c.audio.state === "no_signal"));
const mediamtxDown = computed(() => live.ready && live.ready.body && !live.ready.body.mediamtx);
const registration = computed(() => {
  const n = live.nmos;
  if (!n || !n.enabled) return null;
  if (!n.registry) return { text: "no registry", kind: "neutral" };
  return n.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});
const blocked = computed(() => live.channels.some((c) => resolvePlayback(c).blocked));
const unsavedChannels = computed(() => count((c) => isDirty(c.index)));
const unsavedSettings = computed(() => Object.keys(settingsEdits).length);
const encoders = computed(() => (live.info?.encoder_available || "").replace("nvenc", "NVENC"));

onMounted(() => {
  onHash();
  window.addEventListener("hashchange", onHash);
  startLive();
});
onUnmounted(() => {
  window.removeEventListener("hashchange", onHash);
  stopLive();
});
</script>

<template>
  <header>
    <h1>mxl-webrtc-monitor</h1>
    <span v-if="live.info" class="node">
      {{ live.info.label }} · {{ live.channels.length }} channel{{ live.channels.length === 1 ? "" : "s" }} · {{ encoders }}
    </span>
    <span class="spacer"></span>
    <Pill v-if="live.channels.length" :text="`${running} of ${live.channels.length} running`" :kind="running ? 'ok' : 'neutral'" title="channels whose video is running" />
    <Pill v-if="waiting" :text="`${waiting} waiting`" kind="warn" title="routed, but the domain or flow is not there (yet)" />
    <Pill v-if="noSignal" :text="`${noSignal} no signal`" kind="warn" title="the flow exists but has no new grains" />
    <Pill v-if="mediamtxDown" text="MediaMTX down" kind="bad" title="the MediaMTX API does not answer: no WHEP or HLS" />
    <Pill v-if="registration" :text="registration.text" :kind="registration.kind" title="NMOS registration" />
    <Pill :text="live.connected ? 'live' : 'offline'" :kind="live.connected ? 'ok' : 'bad'" title="/api/v1/events" />
    <span v-if="live.info" class="muted small">v{{ live.info.version }} · MXL {{ live.info.mxl_version.split("-")[0] }} · MediaMTX {{ live.info.mediamtx }}</span>
  </header>
  <div v-if="live.error" class="banner bad">{{ live.error }}</div>
  <div v-else-if="live.everConnected && !live.connected" class="banner warn">Live updates lost. Reconnecting; values refresh every 2 s meanwhile.</div>
  <div v-if="live.config?.restart_required" class="banner warn">
    A setting that applies at start changed. Restart the monitor to apply it.
    <button class="btn small secondary" @click="go('settings')">Settings</button>
  </div>
  <div v-if="blocked" class="banner warn">
    This page is on https and a playback URL is http: the browser blocks it. Set MONITOR_WHEP_PUBLIC_URL and MONITOR_HLS_PUBLIC_URL.
  </div>
  <div v-if="live.actionError" class="banner bad">
    {{ live.actionError }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.actionError = ''">Dismiss</button>
  </div>
  <div v-if="live.notice" class="banner info">
    {{ live.notice }}
    <span class="spacer"></span>
    <button class="btn small secondary" @click="live.notice = ''">Dismiss</button>
  </div>
  <nav>
    <button v-for="t in tabs" :key="t.id" :class="{ active: current === t.id }" :aria-current="current === t.id ? 'page' : undefined" @click="go(t.id)">
      {{ t.label
      }}<span v-if="t.id === 'channels' && unsavedChannels" class="count warn" title="channels with changes not applied">{{ unsavedChannels }}</span
      ><span v-if="t.id === 'settings' && unsavedSettings" class="count warn" title="settings not saved">{{ unsavedSettings }}</span>
    </button>
  </nav>
  <main :class="{ full: tab.full }">
    <div v-show="current === 'multiview'"><MultiviewTab /></div>
    <component :is="tab.component" v-if="tab.component" />
  </main>
</template>
