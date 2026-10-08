<script setup>
// Peak meters per audio input channel (dBFS, −60 to 0) from the WebSocket; the monitored pair is outlined.
import { computed } from "vue";

const props = defineProps({
  peaks: { type: Array, default: () => [] },
  rms: { type: Array, default: () => [] },
  selected: { type: Array, default: () => [] }, // 1-based input channels
  label: { type: String, default: "Audio levels" },
});

const db = (v) => (v > -100 ? `${Number(v).toFixed(1)}` : "-inf");
const bars = computed(() =>
  props.peaks.map((peak, c) => ({
    pct: Math.max(0, Math.min(100, ((Number(peak) + 60) / 60) * 100)),
    kind: peak >= -9 ? "hot" : peak > -17.5 ? "warm" : "",
    sel: props.selected.includes(c + 1),
    title: `input ${c + 1}: peak ${db(peak)} dBFS, RMS ${db(props.rms[c] ?? -120)} dBFS${props.selected.includes(c + 1) ? " (monitored)" : ""}`,
  })),
);
</script>

<template>
  <div class="meters" role="img" :aria-label="label">
    <div v-for="(b, c) in bars" :key="c" class="ch" :class="{ sel: b.sel }" :title="b.title">
      <div class="fill" :class="b.kind" :style="{ height: b.pct + '%' }"></div>
    </div>
    <span v-if="!bars.length" class="none">no audio</span>
  </div>
</template>
