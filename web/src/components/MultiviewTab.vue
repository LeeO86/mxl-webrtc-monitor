<script setup>
// Multiview (§6.1): every channel's player in a grid (automatic, 1, 2×2, 3×3, 4×4); a click on a
// picture shows that channel full size. The tab stays mounted, so other tabs do not stop the players.
import { computed } from "vue";
import ChannelTile from "./ChannelTile.vue";
import Segmented from "./Segmented.vue";
import { live, setLayout, view } from "../store.js";

const LAYOUTS = [
  { value: "auto", label: "Automatic" },
  { value: "1", label: "1×1" },
  { value: "2", label: "2×2" },
  { value: "3", label: "3×3" },
  { value: "4", label: "4×4" },
];
const cols = computed(() => {
  if (view.expanded) return 1;
  if (view.layout !== "auto") return Number(view.layout);
  const n = live.channels.length;
  if (n <= 1) return 1;
  if (n <= 4) return 2;
  if (n <= 9) return 3;
  return 4;
});
// Rows on one screen: all of them (automatic), N for N×N, one for a channel shown full size.
const rows = computed(() => (view.expanded ? 1 : view.layout === "auto" ? Math.max(1, Math.ceil(live.channels.length / cols.value)) : cols.value));
// A tile is its 16:9 picture plus about 9rem of title, status lines and padding; the page above the grid about 12rem.
const gridStyle = computed(() => ({
  gridTemplateColumns: `repeat(${cols.value}, minmax(0, 1fr))`,
  maxWidth: `max(${cols.value * 300}px, calc(${cols.value} * (((100vh - 12rem) / ${rows.value} - 9rem) * 16 / 9 + 1.6rem) + ${cols.value - 1} * .9rem))`,
}));
const toggle = (index) => (view.expanded = view.expanded === index ? 0 : index);
</script>

<template>
  <div class="toolbar">
    <div class="group">
      <span class="caption">Layout</span>
      <Segmented :model-value="view.layout" :options="LAYOUTS" label="Layout" @update:model-value="setLayout" />
    </div>
    <button v-if="view.expanded" class="btn secondary" @click="view.expanded = 0">Show all</button>
    <span class="spacer"></span>
    <span class="muted small">Click a picture for full size. Audio is muted until you unmute a channel.</span>
  </div>
  <div v-if="!live.channels.length" class="empty">Waiting for the monitor…</div>
  <div class="tiles" :style="gridStyle">
    <ChannelTile
      v-for="channel in live.channels"
      v-show="!view.expanded || view.expanded === channel.index"
      :key="channel.index"
      :channel="channel"
      :expanded="view.expanded === channel.index"
      @toggle="toggle(channel.index)"
    />
  </div>
</template>
