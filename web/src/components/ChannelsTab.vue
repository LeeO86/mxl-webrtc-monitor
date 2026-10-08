<script setup>
// Channels (§6.2): the settings of the selected channel as a draft (Apply, Revert), its IS-05 routes
// (read only: routing is IS-05), and its stream (format, encoder, viewers, MediaMTX path, URLs).
import { computed } from "vue";
import ChannelPicker from "./ChannelPicker.vue";
import IdCode from "./IdCode.vue";
import Meters from "./Meters.vue";
import OriginBadge from "./OriginBadge.vue";
import Pill from "./Pill.vue";
import Segmented from "./Segmented.vue";
import { STATE_TEXT, copyText, pairChannels, reasonText, stateKind } from "../api.js";
import { resolvePlayback } from "../player.js";
import { applyDraft, drafts, hasConfigFile, isDirty, live, originOf, receiverOf, revertDraft, selectedChannel, settingsOf } from "../store.js";

const c = selectedChannel;
const d = computed(() => c.value && drafts[c.value.index]?.value);
const dirty = computed(() => c.value && isDirty(c.value.index));
const changed = (k) => c.value && d.value && d.value[k] !== settingsOf(c.value)[k];

// Per-channel keys set by the environment are read-only; the file and default ones can be changed.
const origin = (k) => originOf(`CH${c.value.index}_${k.toUpperCase()}`);
const locked = (k) => origin(k) === "env";

const RANGES = { preview_height: [64, 2160], max_fps: [0, 120], video_bitrate_kbps: [100, 50000], audio_bitrate_kbps: [16, 512] };
const valid = (k) => {
  const v = d.value?.[k];
  if (k === "video_label" || k === "audio_label") return typeof v === "string" && v.trim() !== "";
  if (!RANGES[k]) return true;
  return Number.isInteger(v) && v >= RANGES[k][0] && v <= RANGES[k][1];
};
const allValid = computed(() => d.value && Object.keys(d.value).every(valid));
const cls = (k) => [{ dirty: changed(k), invalid: !valid(k) }];

const pairs = computed(() => {
  const n = c.value?.audio_channels > 0 ? Math.ceil(c.value.audio_channels / 2) : 8;
  const count = Math.max(n, d.value?.audio_pair || 1);
  return Array.from({ length: count }, (_, i) => ({ value: i + 1, label: `${i * 2 + 1}/${i * 2 + 2}` }));
});
const DOWNMIX = [
  { value: "stereo", label: "Stereo" },
  { value: "mono", label: "Mono sum" },
];

const legs = computed(() =>
  c.value
    ? [
        { kind: "video", title: "Video", leg: c.value.video, label: c.value.video_label, rx: receiverOf(c.value.index, "video") },
        { kind: "audio", title: "Audio", leg: c.value.audio, label: c.value.audio_label, rx: receiverOf(c.value.index, "audio") },
      ]
    : [],
);
const urls = computed(() => (c.value ? resolvePlayback(c.value) : null));
async function copy(text) {
  live.notice = (await copyText(text)) ? "Copied." : "Copy failed; select the text and copy it by hand.";
}
</script>

<template>
  <div class="toolbar"><ChannelPicker /></div>
  <div v-if="!c || !d" class="empty">Waiting for the monitor…</div>
  <div v-else class="editor">
    <div class="main">
      <div class="panel">
        <h3>
          Settings
          <span class="spacer"></span>
          <span v-if="dirty" class="pill warn">not applied</span>
        </h3>

        <div class="group-caption">Labels (tile, overlay and API)</div>
        <div class="fields grid">
          <div>
            <label for="ch-vlabel">Video <OriginBadge :origin="origin('video_label')" /></label>
            <input id="ch-vlabel" v-model="d.video_label" :class="cls('video_label')" :disabled="locked('video_label')" />
          </div>
          <div>
            <label for="ch-alabel">Audio <OriginBadge :origin="origin('audio_label')" /></label>
            <input id="ch-alabel" v-model="d.audio_label" :class="cls('audio_label')" :disabled="locked('audio_label')" />
          </div>
        </div>

        <div class="group-caption">Video preview (H.264)</div>
        <div class="fields grid">
          <div>
            <label for="ch-height">Height in pixels (64–2160) <OriginBadge :origin="origin('preview_height')" /></label>
            <input id="ch-height" v-model.number="d.preview_height" type="number" min="64" max="2160" step="2" :class="cls('preview_height')" :disabled="locked('preview_height')" />
          </div>
          <div>
            <label for="ch-fps">Max frame rate (0 = source) <OriginBadge :origin="origin('max_fps')" /></label>
            <input id="ch-fps" v-model.number="d.max_fps" type="number" min="0" max="120" :class="cls('max_fps')" :disabled="locked('max_fps')" />
          </div>
          <div>
            <label for="ch-vbr">Bitrate in kbit/s (100–50000) <OriginBadge :origin="origin('video_bitrate_kbps')" /></label>
            <input id="ch-vbr" v-model.number="d.video_bitrate_kbps" type="number" min="100" max="50000" step="100" :class="cls('video_bitrate_kbps')" :disabled="locked('video_bitrate_kbps')" />
          </div>
        </div>

        <div class="group-caption">Audio (Opus, 48 kHz stereo)</div>
        <div class="fields grid">
          <div>
            <label for="ch-pair">Input pair <OriginBadge :origin="origin('audio_pair')" /></label>
            <select id="ch-pair" v-model.number="d.audio_pair" :class="cls('audio_pair')" :disabled="locked('audio_pair')">
              <option v-for="p in pairs" :key="p.value" :value="p.value">{{ p.label }}</option>
            </select>
          </div>
          <div>
            <label>Downmix <OriginBadge :origin="origin('downmix')" /></label>
            <Segmented v-model="d.downmix" :options="DOWNMIX.map((o) => ({ ...o, disabled: locked('downmix') }))" label="Downmix" />
          </div>
          <div>
            <label for="ch-abr">Bitrate in kbit/s (16–512) <OriginBadge :origin="origin('audio_bitrate_kbps')" /></label>
            <input id="ch-abr" v-model.number="d.audio_bitrate_kbps" type="number" min="16" max="512" step="16" :class="cls('audio_bitrate_kbps')" :disabled="locked('audio_bitrate_kbps')" />
          </div>
        </div>
        <label>Input levels<template v-if="c.audio_channels"> ({{ c.audio_channels }} channels; the selected pair is outlined)</template></label>
        <Meters :peaks="c.meters.peak_dbfs" :rms="c.meters.rms_dbfs" :selected="pairChannels(d.audio_pair)" :label="`${c.audio_label} levels`" />

        <div class="group-caption">Burned-in overlay</div>
        <div class="row tight">
          <label class="check"><input v-model="d.overlay" type="checkbox" :disabled="locked('overlay')" /> Overlay</label>
          <label class="check"><input v-model="d.overlay_label" type="checkbox" :disabled="!d.overlay || locked('overlay_label')" /> Channel label</label>
          <label class="check"><input v-model="d.overlay_source" type="checkbox" :disabled="!d.overlay || locked('overlay_source')" /> Source label</label>
          <label class="check"><input v-model="d.overlay_format" type="checkbox" :disabled="!d.overlay || locked('overlay_format')" /> Format</label>
        </div>

        <p class="note">
          These apply at once. A new preview size, frame rate or bitrate rebuilds the channel's stream: its players reconnect.
          {{ hasConfigFile ? "They are saved to the configuration file." : "MONITOR_CONFIG_FILE is not set: they last until the monitor restarts." }}
          Settings set by an environment variable (ENV) are read-only. The number of channels is <code>MONITOR_CHANNELS</code> on the Settings tab
          (applies after a restart).
        </p>
        <div class="actions">
          <button class="btn secondary" :disabled="!dirty" @click="revertDraft(c.index)">Revert</button>
          <button class="btn" :disabled="!dirty || !allValid" @click="applyDraft(c.index)">Apply to channel {{ c.index }}</button>
        </div>
      </div>
    </div>

    <div class="side">
      <div class="panel">
        <h3>Routes (IS-05)</h3>
        <div v-for="l in legs" :key="l.kind" class="leg">
          <div class="head">
            {{ l.title }}
            <span class="muted small">{{ l.label }}</span>
            <span class="spacer"></span>
            <Pill :text="STATE_TEXT[l.leg.state] || l.leg.state" :kind="stateKind(l.leg.state)" />
          </div>
          <dl class="kv">
            <template v-if="l.leg.reason">
              <dt>Reason</dt>
              <dd>{{ reasonText(l.leg.reason) }}</dd>
            </template>
            <dt>Receiver</dt>
            <dd><IdCode :id="l.rx?.id" /></dd>
            <dt>master_enable</dt>
            <dd>{{ l.leg.master_enable ? "true" : "false" }}</dd>
            <dt>Sender</dt>
            <dd><IdCode :id="l.leg.sender_id" /></dd>
            <dt>Domain</dt>
            <dd><IdCode :id="l.leg.mxl_domain_id" /></dd>
            <dt>Flow</dt>
            <dd><IdCode :id="l.leg.mxl_flow_id" /></dd>
            <template v-if="l.kind === 'video'">
              <dt>Source</dt>
              <dd>{{ c.source_label || "–" }}</dd>
            </template>
          </dl>
        </div>
        <p class="note">Routing is IS-05 only: connect a sender to these receivers from an NMOS controller. Video and audio can come from different senders.</p>
      </div>

      <div class="panel">
        <h3>Stream</h3>
        <dl class="kv">
          <dt>Source format</dt>
          <dd>{{ c.format || "–" }}<span v-if="c.width" class="muted"> ({{ c.width }}×{{ c.height }})</span></dd>
          <dt>Encoder</dt>
          <dd>{{ c.encoder || "–" }} · {{ c.preview_height }}p · {{ c.video_bitrate_kbps }} kbit/s</dd>
          <dt>Audio</dt>
          <dd>{{ c.audio_channels }} input channels · pair {{ pairChannels(c.audio_pair).join("/") }} · {{ c.downmix }}</dd>
          <dt>Viewers</dt>
          <dd>{{ c.viewers.webrtc }} WebRTC · {{ c.viewers.hls }} HLS</dd>
          <dt>MediaMTX</dt>
          <dd>
            <Pill :text="c.mediamtx?.ready ? 'stream ready' : 'no stream'" :kind="c.mediamtx?.ready ? 'ok' : 'warn'" />
            <span v-if="c.mediamtx?.tracks?.length" class="muted small"> {{ c.mediamtx.tracks.join(", ") }}</span>
          </dd>
        </dl>
        <div class="urlrow">
          <span class="muted">WHEP</span>
          <code :title="urls.whep">{{ urls.whep }}</code>
          <button class="btn small secondary" @click="copy(urls.whep)">Copy</button>
        </div>
        <div class="urlrow">
          <span class="muted">HLS</span>
          <code :title="urls.hls">{{ urls.hls }}</code>
          <a :href="urls.hls" target="_blank" rel="noopener" class="small">Open</a>
          <button class="btn small secondary" @click="copy(urls.hls)">Copy</button>
        </div>
      </div>
    </div>
  </div>
</template>
