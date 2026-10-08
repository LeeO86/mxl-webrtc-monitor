// The player of a channel tile (SPECIFICATION.md §6.1): WHEP via the browser's WebRTC, and HLS
// (hls.js) when WebRTC gives no picture within 4 s or fails to connect. A tile starts its player
// again only when playbackKey() changes, never on a status push.
import Hls from "hls.js";

const HLS_AFTER_MS = 4000;
// A WebRTC session that played and then dropped (the channel's stream was rebuilt, MediaMTX
// restarted) starts again after this; so does a player whose HLS failed.
const RETRY_MS = 1500;

function pageUrl(url) {
  try {
    const parsed = new URL(url);
    parsed.hostname = location.hostname;
    return parsed.toString();
  } catch {
    return url;
  }
}

/** The channel's WHEP and HLS URLs as this page reaches them; `blocked` when an https page would load http. */
export function resolvePlayback(channel) {
  const flags = channel.playback?.public || {};
  const whep = flags.whep ? channel.playback.whep : pageUrl(channel.playback.whep);
  const hls = flags.hls ? channel.playback.hls : pageUrl(channel.playback.hls);
  const blocked = location.protocol === "https:" && (String(whep).startsWith("http:") || String(hls).startsWith("http:"));
  return { whep, hls, blocked };
}

// What a player plays: the URLs, the video state or flow, and whether audio is routed (the monitor
// rebuilds the stream with or without an audio track).
export function playbackKey(channel) {
  const resolved = resolvePlayback(channel);
  const audioRouted = Boolean(channel.audio?.master_enable && channel.audio?.mxl_flow_id);
  return JSON.stringify([resolved.whep, resolved.hls, channel.video?.state, channel.video?.mxl_flow_id || "", audioRouted]);
}

/**
 * A player for one <video>. play(channel) (re)starts it, stop() ends it. `onMode` gets "connecting",
 * "webrtc", "hls", "blocked" or "failed".
 */
export function createPlayer(video, onMode) {
  let entry = null;
  let channel = null;
  let retry = 0;

  function stop() {
    clearTimeout(retry);
    if (entry) stopEntry(entry);
    entry = null;
  }
  function again() {
    clearTimeout(retry);
    retry = setTimeout(() => channel && play(channel), RETRY_MS);
  }
  function play(next) {
    stop();
    channel = next;
    const resolved = resolvePlayback(channel);
    if (resolved.blocked && resolved.whep.startsWith("http:") && resolved.hls.startsWith("http:")) {
      onMode("blocked");
      return;
    }
    entry = { video, pc: null, hls: null, nativeHls: false, timer: 0, played: false, stopped: false };
    const current = entry;
    const mode = (m) => {
      if (!current.stopped) onMode(m);
    };
    mode("connecting");
    playWhep(current, resolved, mode, again);
  }
  return { play, stop };
}

async function playWhep(entry, resolved, mode, again) {
  const { video } = entry;
  const current = () => !entry.stopped;
  try {
    const pc = new RTCPeerConnection();
    entry.pc = pc;
    pc.addTransceiver("video", { direction: "recvonly" });
    pc.addTransceiver("audio", { direction: "recvonly" });
    const stream = new MediaStream();
    pc.ontrack = (ev) => {
      stream.addTrack(ev.track);
      if (current()) video.srcObject = stream;
    };
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await new Promise((resolve) => {
      if (pc.iceGatheringState === "complete") resolve();
      else {
        const timer = setTimeout(resolve, 1000);
        pc.addEventListener("icegatheringstatechange", () => {
          if (pc.iceGatheringState === "complete") {
            clearTimeout(timer);
            resolve();
          }
        });
      }
    });
    if (!current()) return;
    const response = await fetch(resolved.whep, {
      method: "POST",
      headers: { "Content-Type": "application/sdp" },
      body: pc.localDescription.sdp,
    });
    if (!response.ok) throw new Error(String(response.status));
    const answer = await response.text();
    if (!current()) return;
    await pc.setRemoteDescription({ type: "answer", sdp: answer });
    entry.timer = setTimeout(() => {
      if (current() && video.readyState < 2) startHls(entry, resolved.hls, mode, again);
    }, HLS_AFTER_MS);
    pc.onconnectionstatechange = () => {
      if (!current()) return;
      if (pc.connectionState === "connected") {
        clearTimeout(entry.timer);
        entry.played = true;
        mode("webrtc");
      } else if (pc.connectionState === "failed" || pc.connectionState === "disconnected") {
        // Played before: WebRTC works here, the stream went away. Never played: try HLS.
        if (entry.played) {
          mode("connecting");
          stopEntry(entry);
          again();
        } else {
          startHls(entry, resolved.hls, mode, again);
        }
      }
    };
    await video.play().catch(() => {});
  } catch {
    if (current()) startHls(entry, resolved.hls, mode, again);
  }
}

// Falls back from WHEP to HLS: the peer connection's stream must leave the element first.
function startHls(entry, url, mode, again) {
  if (entry.hls || entry.nativeHls) return;
  clearTimeout(entry.timer);
  if (entry.pc) {
    entry.pc.close();
    entry.pc = null;
  }
  const video = entry.video;
  video.srcObject = null;
  mode("hls");
  const failed = () => {
    if (entry.stopped) return;
    mode("failed");
    stopEntry(entry);
    again();
  };
  if (video.canPlayType("application/vnd.apple.mpegurl")) {
    entry.nativeHls = true;
    video.src = url;
    video.onerror = failed;
    video.play().catch(() => {});
    return;
  }
  const hls = new Hls({ lowLatencyMode: true, enableWorker: true });
  entry.hls = hls;
  hls.loadSource(url);
  hls.attachMedia(video);
  hls.on(Hls.Events.MANIFEST_PARSED, () => video.play().catch(() => {}));
  hls.on(Hls.Events.ERROR, (_, data) => {
    if (data.fatal) failed();
  });
}

function stopEntry(entry) {
  entry.stopped = true;
  clearTimeout(entry.timer);
  if (entry.pc) entry.pc.close();
  if (entry.hls) entry.hls.destroy();
  entry.video.onerror = null;
  entry.video.srcObject = null;
  if (entry.nativeHls) entry.video.removeAttribute("src");
}
