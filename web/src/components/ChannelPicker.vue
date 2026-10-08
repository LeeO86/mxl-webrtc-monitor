<script setup>
// The channel a page edits, with each channel's video state. Kept in this browser.
import { computed } from "vue";
import Segmented from "./Segmented.vue";
import { STATE_TEXT } from "../api.js";
import { isDirty, live, selectChannel, selectedChannel } from "../store.js";

const options = computed(() =>
  live.channels.map((c) => ({ value: c.index, label: c.video_label, state: c.video.state, dirty: isDirty(c.index) })),
);
</script>

<template>
  <div class="group">
    <span class="caption">Channel</span>
    <Segmented :options="options" :model-value="selectedChannel?.index" label="Channel" @update:model-value="selectChannel">
      <template #default="{ option }">
        {{ option.label }}<span v-if="option.dirty" title="not applied">*</span><span class="state-tag" :class="option.state">{{ STATE_TEXT[option.state] }}</span>
      </template>
    </Segmented>
  </div>
</template>
