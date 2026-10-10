<script setup>
// Operator-screen widget (SPECIFICATION.md §6.6): /widget/channel?ch=<n>[&labels=][&meters=][&theme=] is one
// channel tile without the app around it, fed by this monitor's own API (same origin). It posts
// {type: "widget-ready"} and {type: "widget-size", w, h} to the page that frames it.
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import ChannelTile from "./ChannelTile.vue";
import { live, startLive, stopLive } from "../store.js";

const params = new URLSearchParams(location.search);
const flag = (name) => !["false", "0"].includes(params.get(name));
const ch = Number(params.get("ch"));
const theme = params.get("theme");
if (theme) document.documentElement.dataset.theme = theme;
// A frame is see-through only when its color scheme matches its parent's; without the meta it is the default.
if (theme === "transparent") document.querySelector('meta[name="color-scheme"]')?.remove();

const channel = computed(() => live.channels.find((c) => c.index === ch));
const root = ref(null);
let ready = false;
let observer = null;

function post(message) {
  if (window.parent !== window) window.parent.postMessage(message, "*");
}
function postSize() {
  const rect = root.value.getBoundingClientRect();
  post({ type: "widget-size", w: Math.round(rect.width), h: Math.round(rect.height) });
}
watch(
  channel,
  (c) => {
    if (!c || ready) return;
    ready = true;
    post({ type: "widget-ready" });
    postSize();
  },
  { flush: "post" },
);
onMounted(() => {
  startLive();
  observer = new ResizeObserver(() => ready && postSize());
  observer.observe(root.value);
});
onUnmounted(() => {
  observer?.disconnect();
  stopLive();
});
</script>

<template>
  <div ref="root" class="widget-root">
    <ChannelTile v-if="channel" :channel="channel" widget :labels="flag('labels')" :meters="flag('meters')" />
    <div v-else class="empty">{{ live.error || `Waiting for channel ${ch}…` }}</div>
  </div>
</template>
