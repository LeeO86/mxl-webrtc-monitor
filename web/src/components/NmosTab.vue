<script setup>
// NMOS (§4, §6.2): the node, its registration, and every receiver with its active IS-05
// parameters and subscription. Read only: routing is done by an NMOS controller.
import { computed } from "vue";
import IdCode from "./IdCode.vue";
import Pill from "./Pill.vue";
import { STATE_TEXT, reasonText, stateKind } from "../api.js";
import { live } from "../store.js";

const n = computed(() => live.nmos);
const values = computed(() => live.config?.values || {});
const base = computed(() => (n.value ? `http://${values.value.NMOS_HOST_ADDRESS || location.hostname}:${n.value.port}` : ""));
const registration = computed(() => {
  if (!n.value?.enabled) return { text: "NMOS off", kind: "neutral" };
  if (!n.value.registry) return { text: "no registry", kind: "neutral" };
  return n.value.registered ? { text: "registered", kind: "ok" } : { text: "not registered", kind: "warn" };
});
// The channel and its leg (for the reason) of a receiver: GET /api/v1/nmos lists video then audio per channel.
const rows = computed(() =>
  (n.value?.receivers || []).map((rx, i) => {
    const channel = live.channels.find((c) => c.index === Math.floor(i / 2) + 1);
    return { rx, channel, leg: channel?.[rx.kind] };
  }),
);
</script>

<template>
  <div v-if="!n" class="empty">Loading…</div>
  <template v-else>
    <div class="grid two">
      <div class="panel">
        <h3>Node <span class="spacer"></span><Pill :text="registration.text" :kind="registration.kind" /></h3>
        <dl class="kv">
          <dt>Label</dt>
          <dd>{{ live.info?.label || "–" }}</dd>
          <dt>Node id</dt>
          <dd><code>{{ n.node_id }}</code></dd>
          <dt>Device</dt>
          <dd>{{ n.device_label }} <code>{{ n.device_id }}</code></dd>
          <dt>Address</dt>
          <dd>{{ values.NMOS_HOST_ADDRESS || "–" }}:{{ n.port }} (node and connection API; WebSocket {{ n.port + 1 }})</dd>
          <dt>Registry</dt>
          <dd>
            {{ n.registry ? `${n.registry}:${n.registry_port}` : "none: not registered" }}
            <span v-if="n.registry" class="muted">(query {{ values.NMOS_QUERY_ADDRESS }}:{{ values.NMOS_QUERY_PORT }})</span>
          </dd>
          <dt>DNS-SD</dt>
          <dd>{{ n.dns_sd ? "on" : "off" }}</dd>
          <dt>Seed</dt>
          <dd><code>{{ values.NMOS_SEED || "–" }}</code></dd>
        </dl>
        <div class="note">
          Raw resources: <a :href="`${base}/x-nmos/node/v1.3/self`" target="_blank" rel="noopener">self</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/devices`" target="_blank" rel="noopener">devices</a> ·
          <a :href="`${base}/x-nmos/node/v1.3/receivers`" target="_blank" rel="noopener">receivers</a> ·
          <a :href="`${base}/x-nmos/connection/v1.2/single/receivers`" target="_blank" rel="noopener">connection</a>
        </div>
      </div>
      <div class="panel">
        <h3>How routing works</h3>
        <p class="note" style="margin-top: 0">
          Each channel is a pair of BCP-007-03 receivers (transport <code>urn:x-nmos:transport:mxl</code>) grouped as
          <code>Monitor &lt;n&gt;:Video</code> and <code>:Audio</code>. A controller activates a sender's <code>mxl_domain_id</code> and
          <code>mxl_flow_id</code> on a receiver with IS-05. The monitor accepts a flow that does not exist yet (state waiting) and starts when it
          appears, so a fabrics mirror can follow. <code>master_enable</code> false stops the channel (not routed). The routes are kept across a
          restart. This page has no source picker.
        </p>
      </div>
    </div>

    <div class="panel">
      <h3>Receivers</h3>
      <table>
        <thead>
          <tr><th>Channel</th><th>Essence</th><th>Channel label</th><th>Receiver id</th><th>master_enable</th><th>State</th><th>Sender</th><th>Domain</th><th>Flow</th><th></th></tr>
        </thead>
        <tbody>
          <tr v-for="(r, i) in rows" :key="r.rx.id" :class="{ dim: !r.rx.active }">
            <td>{{ i % 2 === 0 ? Math.floor(i / 2) + 1 : "" }}</td>
            <td>{{ r.rx.kind === "video" ? "Video" : "Audio" }}</td>
            <td>{{ r.rx.label }}</td>
            <td><IdCode :id="r.rx.id" /></td>
            <td><Pill :text="r.rx.active ? 'true' : 'false'" :kind="r.rx.active ? 'ok' : 'neutral'" /></td>
            <td class="nowrap">
              <Pill :text="STATE_TEXT[r.rx.state] || r.rx.state" :kind="stateKind(r.rx.state)" />
              <span v-if="r.leg?.reason" class="muted small"> {{ reasonText(r.leg.reason) }}</span>
            </td>
            <td><IdCode :id="r.rx.sender_id" /></td>
            <td><IdCode :id="r.rx.mxl_domain_id" /></td>
            <td><IdCode :id="r.rx.mxl_flow_id" /></td>
            <td class="small"><a :href="`${base}/x-nmos/connection/v1.2/single/receivers/${r.rx.id}/active`" target="_blank" rel="noopener">active</a></td>
          </tr>
        </tbody>
      </table>
      <p class="note">Hover an id for all of it. The sender is the one the controller named in the activation (empty when it gave none).</p>
    </div>
  </template>
</template>
