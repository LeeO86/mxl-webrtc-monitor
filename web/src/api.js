// REST client for the monitor API (SPECIFICATION.md §6.3, §6.4) and display helpers.

async function request(path, { method = "GET", body, text = false } = {}) {
  const headers = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  const resp = await fetch(path, {
    method,
    headers,
    body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await resp.text();
  let data = raw;
  if (!text) {
    try {
      data = raw ? JSON.parse(raw) : null;
    } catch {
      data = raw;
    }
  }
  if (!resp.ok) {
    const reason = data && typeof data === "object" ? data.error : String(data || "").slice(0, 200);
    throw new Error(reason || `HTTP ${resp.status}`);
  }
  return data;
}

export const api = {
  get: (path) => request(path),
  text: (path) => request(path, { text: true }),
  post: (path, body) => request(path, { method: "POST", body: body ?? {} }),
  put: (path, body) => request(path, { method: "PUT", body }),
  patch: (path, body) => request(path, { method: "PATCH", body }),
};

/** HTTP status and JSON body of a probe (/livez, /readyz, /statusz) without throwing; status 0 when it does not answer. */
export async function probe(path) {
  try {
    const resp = await fetch(path, { cache: "no-store" });
    let body = null;
    try {
      body = await resp.json();
    } catch {
      /* not JSON */
    }
    return { status: resp.status, body };
  } catch {
    return { status: 0, body: null };
  }
}

/** The first 8 characters of an id; the whole id goes into a title. */
export const shortId = (id) => (id ? String(id).slice(0, 8) : "–");

export const clone = (v) => JSON.parse(JSON.stringify(v));

/** JSON with sorted keys, to compare a draft with the status. */
export function canon(value) {
  return JSON.stringify(value, (key, v) =>
    v && typeof v === "object" && !Array.isArray(v) ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, v[k]])) : v,
  );
}

/** Copies text; falls back to a hidden textarea on plain http, where navigator.clipboard is missing. */
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.opacity = "0";
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand("copy");
    area.remove();
    return ok;
  }
}

export function download(name, text, type = "application/json") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

/** Channel leg states (§5.2) as words, and their pill colour. */
export const STATE_TEXT = { running: "running", no_signal: "no signal", waiting: "waiting", not_routed: "not routed" };
export const stateKind = (state) => ({ running: "ok", no_signal: "warn", waiting: "warn" })[state] || "neutral";
const REASON_TEXT = { domain_not_found: "domain not found", flow_not_found: "flow not found" };
export const reasonText = (reason) => REASON_TEXT[reason] || (reason || "").replaceAll("_", " ");

/** Input channels (1-based) of an audio pair. */
export const pairChannels = (pair) => [(pair - 1) * 2 + 1, (pair - 1) * 2 + 2];

export function fmtBitrate(bps) {
  if (!Number.isFinite(bps)) return "–";
  if (bps >= 1e6) return `${(bps / 1e6).toFixed(2)} Mbit/s`;
  return `${Math.round(bps / 1e3)} kbit/s`;
}

/**
 * Per-channel numbers from /metrics (Prometheus text): `grains_read_total`, `grains_dropped_total_queue_full`,
 * `grains_dropped_total_too_late`, `resyncs_total`, `read_lag_grains`, `encode_fps`, `encode_latency_seconds_sum`
 * and `_count`, `encoder_fallbacks_total`, `output_bitrate_bps`. Keyed by channel number.
 */
export function parseMetrics(text) {
  const out = {};
  for (const line of text.split("\n")) {
    const m = /^mxl_webrtc_monitor_(\w+)\{([^}]*)\} (\S+)$/.exec(line);
    if (!m) continue;
    const labels = Object.fromEntries([...m[2].matchAll(/(\w+)="([^"]*)"/g)].map((x) => [x[1], x[2]]));
    if (!labels.channel || labels.le) continue;
    const entry = (out[labels.channel] ||= {});
    entry[labels.reason ? `${m[1]}_${labels.reason}` : m[1]] = Number(m[3]);
  }
  return out;
}
