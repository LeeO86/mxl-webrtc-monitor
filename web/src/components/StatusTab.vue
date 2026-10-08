<script setup>
// Status (§6.4, §8): health probes, versions, MediaMTX, and per channel the counters of /metrics
// (reads, drops, resyncs, lag, encoding, bitrate, fallbacks) with viewers and the MediaMTX path.
import { computed, onMounted, onUnmounted, ref } from "vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, api, fmtBitrate, parseMetrics, probe, stateKind } from "../api.js";
import { live } from "../store.js";

const health = ref({ livez: null, statusz: null });
const metrics = ref({});
let timer = null;

async function load() {
  const [livez, statusz] = await Promise.all([probe("/livez"), probe("/statusz")]);
  health.value = { livez, statusz };
  try {
    metrics.value = parseMetrics(await api.text("/metrics"));
  } catch {
    /* keep the last values */
  }
}
onMounted(() => {
  load();
  timer = setInterval(load, 2000);
});
onUnmounted(() => clearInterval(timer));

const code = (p) => ({ text: p?.status ? String(p.status) : "no answer", kind: p?.status === 200 ? "ok" : p?.status ? "warn" : "bad" });
const yes = (v) => ({ text: v ? "yes" : "no", kind: v ? "ok" : "bad" });
const ready = computed(() => live.ready?.body || {});
const values = computed(() => live.config?.values || {});
const info = computed(() => live.info || {});
const pathsReady = computed(() => live.channels.filter((c) => c.mediamtx?.ready).length);
const m = (c) => metrics.value[String(c.index)] || {};
const latencyMs = (c) => {
  const x = m(c);
  return x.encode_latency_seconds_count > 0 ? ((x.encode_latency_seconds_sum / x.encode_latency_seconds_count) * 1000).toFixed(1) : "–";
};
const num = (v, digits = 0) => (Number.isFinite(v) ? v.toFixed(digits) : "–");
</script>

<template>
  <div class="grid fit">
    <div class="panel">
      <h3>Health</h3>
      <dl class="kv">
        <dt>/livez</dt>
        <dd><Pill :text="code(health.livez).text" :kind="code(health.livez).kind" /></dd>
        <dt>/readyz</dt>
        <dd><Pill :text="code(live.ready).text" :kind="code(live.ready).kind" /></dd>
        <dt>MXL root mounted</dt>
        <dd><Pill v-bind="yes(ready.mxl_root)" /> <code>{{ values.MXL_DOMAIN_SCAN_PATH }}</code></dd>
        <dt>NMOS registered</dt>
        <dd><Pill v-bind="yes(ready.nmos)" /></dd>
        <dt>MediaMTX API</dt>
        <dd><Pill :text="ready.mediamtx ? 'answers' : 'no answer'" :kind="ready.mediamtx ? 'ok' : 'bad'" /></dd>
        <dt>Restart required</dt>
        <dd><Pill :text="health.statusz?.body?.restart_required ? 'yes' : 'no'" :kind="health.statusz?.body?.restart_required ? 'warn' : 'ok'" /></dd>
        <dt>Live updates</dt>
        <dd><Pill :text="live.connected ? 'connected' : 'reconnecting'" :kind="live.connected ? 'ok' : 'warn'" /></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>Versions</h3>
      <dl class="kv">
        <dt>Monitor</dt>
        <dd>{{ info.version || "–" }}</dd>
        <dt>MXL</dt>
        <dd>{{ info.mxl_version || "–" }}</dd>
        <dt>nmos-cpp</dt>
        <dd><code :title="info.nmos_cpp">{{ (info.nmos_cpp || "–").slice(0, 12) }}</code></dd>
        <dt>GStreamer</dt>
        <dd>{{ info.gstreamer || "–" }}</dd>
        <dt>MediaMTX</dt>
        <dd>{{ info.mediamtx || "–" }} <span class="muted">(examples pin {{ info.mediamtx_pin || "–" }})</span></dd>
        <dt>Encoders</dt>
        <dd>{{ info.encoder_available || "–" }} <span class="muted">(ENCODER={{ values.ENCODER || "–" }})</span></dd>
      </dl>
    </div>
    <div class="panel">
      <h3>MediaMTX</h3>
      <dl class="kv">
        <dt>Streams ready</dt>
        <dd>{{ pathsReady }} of {{ live.channels.length }}</dd>
        <dt>API</dt>
        <dd><code>{{ values.MEDIAMTX_API_URL }}</code></dd>
        <dt>RTSP ingest</dt>
        <dd><code>{{ values.MEDIAMTX_RTSP_URL }}</code></dd>
        <dt>WHEP</dt>
        <dd>{{ values.MONITOR_WHEP_PUBLIC_URL || `port ${values.MEDIAMTX_WHEP_PORT}` }}</dd>
        <dt>HLS</dt>
        <dd>{{ values.MONITOR_HLS_PUBLIC_URL || `port ${values.MEDIAMTX_HLS_PORT}` }}</dd>
        <dt>ICE</dt>
        <dd>{{ values.MONITOR_PUBLIC_IP }}:{{ values.MEDIAMTX_ICE_UDP_PORT }} UDP and TCP</dd>
        <dt>Metrics port</dt>
        <dd>{{ values.MEDIAMTX_METRICS_PORT === "0" ? "API port + 1" : values.MEDIAMTX_METRICS_PORT }}</dd>
      </dl>
    </div>
  </div>

  <div class="panel">
    <h3>Channels</h3>
    <table>
      <thead>
        <tr>
          <th>Channel</th>
          <th>Video</th>
          <th>Audio</th>
          <th>Encoder</th>
          <th class="num">Encode fps</th>
          <th class="num" title="mean since start">Encode ms</th>
          <th class="num">Output</th>
          <th class="num">Grains read</th>
          <th class="num" title="encoder queue full / too late">Dropped</th>
          <th class="num">Resyncs</th>
          <th class="num" title="grains behind the flow's head">Lag</th>
          <th class="num" title="NVENC to x264">Fallbacks</th>
          <th class="num">Viewers</th>
          <th>MediaMTX</th>
        </tr>
      </thead>
      <tbody>
        <tr v-for="c in live.channels" :key="c.index">
          <td><strong>{{ c.index }}</strong> <span class="muted small">{{ c.video_label }}</span></td>
          <td><Pill :text="STATE_TEXT[c.video.state]" :kind="stateKind(c.video.state)" /></td>
          <td><Pill :text="STATE_TEXT[c.audio.state]" :kind="stateKind(c.audio.state)" /></td>
          <td>{{ c.encoder || "–" }}</td>
          <td class="num">{{ num(m(c).encode_fps, 1) }}</td>
          <td class="num">{{ latencyMs(c) }}</td>
          <td class="num">{{ fmtBitrate(m(c).output_bitrate_bps) }}</td>
          <td class="num">{{ num(m(c).grains_read_total) }}</td>
          <td class="num">{{ num(m(c).grains_dropped_total_queue_full) }} / {{ num(m(c).grains_dropped_total_too_late) }}</td>
          <td class="num">{{ num(m(c).resyncs_total) }}</td>
          <td class="num">{{ num(m(c).read_lag_grains, 1) }}</td>
          <td class="num"><Pill :text="num(m(c).encoder_fallbacks_total)" :kind="m(c).encoder_fallbacks_total ? 'warn' : 'ok'" /></td>
          <td class="num" :title="`${c.viewers.webrtc} WebRTC, ${c.viewers.hls} HLS`">{{ c.viewers.webrtc }} / {{ c.viewers.hls }}</td>
          <td class="nowrap">
            <Pill :text="c.mediamtx?.ready ? 'ready' : 'no stream'" :kind="c.mediamtx?.ready ? 'ok' : 'warn'" />
            <span class="muted small"> {{ (c.mediamtx?.tracks || []).join(", ") }}</span>
          </td>
        </tr>
      </tbody>
    </table>
    <p class="note">
      Viewers are WebRTC / HLS readers in MediaMTX. Counters since the monitor started; all of them, with the latency histogram:
      <a href="/metrics" target="_blank">/metrics</a> (Prometheus).
    </p>
  </div>
</template>
