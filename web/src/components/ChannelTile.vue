<script setup>
// One channel in the multiview: its player (WHEP, HLS fallback), state, source, audio meters with
// the monitored pair, mute, and the stream's viewers. A click on the picture shows it full size.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import Meters from "./Meters.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, pairChannels, reasonText, stateKind } from "../api.js";
import { createPlayer, playbackKey } from "../player.js";

const props = defineProps({
  channel: { type: Object, required: true },
  expanded: { type: Boolean, default: false },
});
defineEmits(["toggle"]);

const video = ref(null);
const mode = ref("connecting");
const muted = ref(true);
let player = null;

const key = computed(() => playbackKey(props.channel));
function start() {
  if (!player) return;
  video.value.muted = muted.value;
  player.play(props.channel);
}
watch(key, start);
onMounted(() => {
  player = createPlayer(video.value, (m) => (mode.value = m));
  start();
});
onUnmounted(() => player?.stop());

function toggleMute() {
  muted.value = !muted.value;
  video.value.muted = muted.value;
  if (!muted.value) video.value.play().catch(() => {});
}

const MODES = {
  connecting: { text: "connecting", kind: "neutral" },
  webrtc: { text: "WebRTC", kind: "ok" },
  hls: { text: "HLS", kind: "warn" },
  blocked: { text: "blocked", kind: "bad" },
  failed: { text: "failed", kind: "bad" },
};
const c = computed(() => props.channel);
const source = computed(() => {
  const ch = c.value;
  if (ch.video.state === "not_routed") return "video not routed";
  if (ch.video.state === "waiting") return `waiting: ${reasonText(ch.video.reason) || "no flow yet"}`;
  return [ch.source_label, ch.format].filter(Boolean).join(" · ") || "–";
});
const audioText = computed(() => {
  const a = c.value.audio;
  if (a.state === "not_routed") return "audio not routed";
  const pair = pairChannels(c.value.audio_pair);
  return `audio ${STATE_TEXT[a.state]} · ${c.value.audio_channels} ch · pair ${pair.join("/")}${c.value.downmix === "mono" ? " mono" : ""}`;
});
</script>

<template>
  <div class="panel tile" :class="{ expanded }">
    <h3>
      <span class="idx">{{ c.index }}</span>
      <span class="name" :title="`Channel ${c.index}: ${c.video_label} / ${c.audio_label}`">{{ c.video_label }}</span>
      <span class="spacer"></span>
      <span class="state-tag" :class="c.video.state">{{ STATE_TEXT[c.video.state] || c.video.state }}</span>
    </h3>
    <div class="picture" :title="expanded ? 'Back to the grid' : 'Full size'" @click="$emit('toggle')">
      <video ref="video" autoplay playsinline muted></video>
      <span v-if="mode === 'blocked'" class="ov bl hint">Playback blocked on https: set MONITOR_WHEP_PUBLIC_URL and MONITOR_HLS_PUBLIC_URL.</span>
    </div>
    <div class="srcline" :title="source">{{ source }}</div>
    <div class="statusline">
      <button class="btn small" :class="{ secondary: muted }" :aria-pressed="!muted" :title="muted ? 'Listen to this channel' : 'Mute'" @click="toggleMute">
        {{ muted ? "Unmute" : "Mute" }}
      </button>
      <Meters :peaks="c.meters.peak_dbfs" :rms="c.meters.rms_dbfs" :selected="pairChannels(c.audio_pair)" :label="`${c.audio_label} levels`" />
    </div>
    <div class="statusline">
      <span :class="{ warn: stateKind(c.audio.state) === 'warn' }">{{ audioText }}</span>
      <span>{{ c.encoder || "–" }} · {{ c.preview_height }}p</span>
      <span :title="`${c.viewers.webrtc} WebRTC, ${c.viewers.hls} HLS`">viewers {{ c.viewers.webrtc + c.viewers.hls }}</span>
      <span class="spacer"></span>
      <Pill :text="MODES[mode].text" :kind="MODES[mode].kind" title="what this page plays" />
    </div>
  </div>
</template>
