"use strict";

const $ = (id) => document.getElementById(id);

const MODE_HINTS = {
  paced: "按帧率节流送帧：每帧间隔到达，回执通常在提交后立即出现；用于观察稳态延迟。",
  burst: "尽可能快地连续送帧：用有限表面池制造背压，观察 FILLING/QUEUED 的阻塞与最大在途数量。",
  late: "延迟回执：硬件仍在读取表面时不得改写，池会被占满，送帧必须等待回执（服务端为软件模型加入的定时延迟，非设备时延）。",
  guard: "所有权守卫：对仍被编码器持有的表面尝试写入必须被拒绝；界面展示拒绝计数。",
  corrupt: "注入故障：故意破坏其中一帧的帧号标记，验证解码核对能发现错误而不是盲目通过。",
};

const state = {
  latest: null,
  devices: null,
  follow: true,
  selectedSeq: null,
  pollTimer: null,
  imgBusy: false,
  lastKey: "",
  imageSeq: { src: null, out: null },
};

function fmtMs(v) {
  if (v === null || v === undefined || v < 0) return "—";
  return v >= 100 ? v.toFixed(0) + " ms" : v.toFixed(1) + " ms";
}
function fmtBytes(n) {
  if (!n && n !== 0) return "—";
  if (n > 1024 * 1024) return (n / 1048576).toFixed(2) + " MiB";
  if (n > 1024) return (n / 1024).toFixed(1) + " KiB";
  return n + " B";
}
function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

async function api(path, opts) {
  const r = await fetch(path, opts);
  let body = null;
  const ct = r.headers.get("content-type") || "";
  if (ct.includes("application/json")) body = await r.json();
  if (!r.ok) throw new Error((body && body.error) || r.status + " " + r.statusText);
  return body;
}

async function loadDevices() {
  try {
    const d = await api("/api/devices");
    state.devices = d;
    const sel = $("fBackend");
    sel.innerHTML = "";
    const sw = document.createElement("option");
    sw.value = "openh264";
    sw.textContent = "OpenH264 软件对照（接口模型）";
    sel.appendChild(sw);
    if (d.v4l2Available) {
      for (const dev of d.devices) {
        if (!dev.h264Capture) continue;
        const o = document.createElement("option");
        o.value = "v4l2";
        o.textContent = "V4L2 硬件：" + dev.path + " (" + dev.driver + ")";
        o.dataset.device = dev.path;
        sel.appendChild(o);
      }
    }
    const devSel = $("fDevice");
    devSel.innerHTML = "";
    const any = document.createElement("option");
    any.value = "";
    any.textContent = d.v4l2Available ? "自动选择可用节点" : "无 V4L2 M2M 节点";
    devSel.appendChild(any);
    for (const dev of d.devices) {
      const o = document.createElement("option");
      o.value = dev.path;
      o.textContent = dev.path + "  " + dev.driver + " · " + dev.note;
      devSel.appendChild(o);
    }
    devSel.disabled = !d.v4l2Available;
    renderCapsHint();
  } catch (e) {
    console.error(e);
  }
}

function selectedBackendHint() {
  const sel = $("fBackend");
  const v = sel.value;
  if (v === "v4l2") {
    const opt = sel.selectedOptions[0];
    const dev = opt && opt.dataset.device;
    if (dev) $("fDevice").value = dev;
  } else {
    $("fDevice").value = "";
  }
  renderCapsHint();
}

function renderCapsHint() {
  const d = state.devices;
  const box = $("capsBox");
  if (!d) { box.textContent = ""; return; }
  const backend = $("fBackend").value;
  let html = "";
  if (backend === "v4l2" && d.v4l2Available) {
    html += `<div><b>硬件后端</b>：使用真实 V4L2 memory-to-memory 接口，格式由 S_FMT 协商，步幅取自 G_FMT。</div>`;
    html += `<div><b>表面释放</b>：仅当 DQBUF(OUTPUT) 返回缓冲后才发 SurfaceReleased；在此之前禁止改写。</div>`;
  } else if (backend === "v4l2") {
    html += `<div><b>硬件不可用</b>：本机未发现可用的 V4L2 M2M H.264 节点（${esc(d.v4l2Reason || "")}）。</div>`;
    html += `<div>请切换到 OpenH264 软件对照运行；软件路径会明确标记为“硬件未覆盖”，不会伪造硬件测速。</div>`;
  } else {
    html += `<div><b>软件对照</b>：真实 OpenH264 编码与独立解码核对，用于验证整条送帧链路；<b>不是</b>硬件吞吐量测量。</div>`;
    html += `<div>该后端也是异步回执的接口模型：码流同步可得，输入表面按“延迟回执”方式释放，用于演示背压与所有权约束。</div>`;
  }
  const sw = d.software;
  if (sw) html += `<div><b>软件编码器</b>：${esc(sw.name)}</div>`;
  box.innerHTML = html;
}

function readConfig() {
  const [w, h] = $("fSize").value.split("x").map(Number);
  const backend = $("fBackend").value;
  const mode = $("fMode").value;
  return {
    backend,
    mode,
    width: w,
    height: h,
    fps: Number($("fFps").value),
    bitrate: Number($("fBitrate").value),
    gop: Number($("fGop").value),
    pool: Number($("fPool").value),
    frames: Number($("fFrames").value),
    verify: 1,
    receiptLagMs: mode === "late" ? Number($("fLag").value) : 0,
    device: backend === "v4l2" ? $("fDevice").value : "",
    format: $("fFormat").value,
  };
}

function setRunning(running) {
  $("btnStart").disabled = running;
  $("btnStop").disabled = !running;
  for (const id of ["fBackend", "fMode", "fSize", "fFps", "fBitrate", "fGop", "fPool", "fFrames", "fDevice", "fFormat"]) {
    $(id).disabled = running;
  }
  $("fLag").disabled = running || $("fMode").value !== "late";
}

async function start() {
  const c = readConfig();
  const q = new URLSearchParams(c).toString();
  try {
    await api("/api/start?" + q, { method: "POST" });
    state.follow = true;
    state.selectedSeq = null;
    state.imageSeq = { src: null, out: null };
    $("btnLive").setAttribute("aria-pressed", "true");
    setRunning(true);
  } catch (e) {
    $("vNote").textContent = "启动失败：" + e.message;
  }
  pollNow();
}

async function stop() {
  try { await api("/api/stop", { method: "POST" }); } catch (e) { console.error(e); }
}

function statusChip(el, text, cls) {
  el.textContent = text;
  el.className = "chip " + cls;
}

function stateChip(s) {
  switch (s) {
    case "running": case "draining": return ["运行中 · " + s, "chip-good"];
    case "starting": return ["启动中", "chip-warn"];
    case "completed": return ["已完成", "chip-good"];
    case "failed": return ["失败", "chip-bad"];
    case "stopped": return ["已停止", "chip-warn"];
    default: return ["空闲", "chip-muted"];
  }
}

function renderSurfaces(snap) {
  const wrap = $("surfCards");
  const surf = snap.surfaces || [];
  if (!surf.length) {
    wrap.innerHTML = '<div class="meta">尚无表面：运行后显示每个真实表面的状态、几何与释放条件。</div>';
    $("surfSummary").textContent = "";
    return;
  }
  const held = surf.filter((s) => s.state !== "FREE").length;
  $("surfSummary").textContent =
    `共 ${surf.length} 个表面 · 在途 ${held} · 池上限 ${snap.caps.maxPool || "—"} · 映射 ${fmtBytes(snap.mappedBytes)}`;
  wrap.innerHTML = surf
    .map((s) => {
      const dev = s.deviceMemory ? "设备映射" : "主机内存";
      return `<div class="surf-card">
        <div class="sc-head">
          <span class="sc-title">Surface ${s.index}</span>
          <span class="sc-state ${esc(s.state)}">${esc(s.state)}</span>
        </div>
        <div class="sc-cells">
          <span>帧号 <b>${s.seq >= 0 ? s.seq : "—"}</b></span>
          <span>复用 <b>${s.reuse}</b></span>
          <span>填充 <b>${s.fills}</b></span>
          <span>大小 <b>${fmtBytes(s.bytes)}</b></span>
          <span>格式 <b>${esc(s.fmt)}</b></span>
          <span>${dev}</span>
          <span>可见 <b>${s.visibleW}×${s.visibleH}</b></span>
          <span>编码区 <b>${s.codedW}×${s.codedH}</b></span>
          <span>步幅 <b>${s.stride0}/${s.stride1}</b></span>
          <span>stride2 <b>${s.stride2 || "—"}</b></span>
        </div>
        <div class="sc-cond">回收条件：${esc(s.releaseCond || "—")}</div>
      </div>`;
    })
    .join("");
}

function renderFrames(snap) {
  const frames = snap.frames || [];
  const tbody = $("frameTable").querySelector("tbody");
  const m = snap.metrics || {};
  $("frameSummary").textContent =
    `提交 ${m.submitted || 0} · 码流 ${m.bitstreams || 0} · 核对通过 ${m.verified || 0}` +
    (m.detected ? ` · 检出故障 ${m.detected}` : "") +
    ` · 失败 ${m.failed || 0} · 乱序 ${m.outOfOrder || 0}`;

  const sel = $("frameSel");
  const prevVal = sel.value;
  const withOut = frames.filter((f) => f.hasOut || f.hasSrc);
  const want = withOut.length ? withOut : frames;
  const sig = want.map((f) => f.seq + ":" + f.status).join(",");
  if (sel.dataset.sig !== sig) {
    sel.dataset.sig = sig;
    sel.innerHTML = want
      .map((f) => `<option value="${f.seq}">帧 ${f.seq} · ${esc(f.status)}</option>`)
      .join("");
    if (prevVal && want.some((f) => String(f.seq) === prevVal)) sel.value = prevVal;
  }
  // In follow mode the pictures track the newest frame, so the selector must
  // show that same frame instead of keeping whatever was selected before.
  if (state.follow && want.length) {
    const newest = String(want[want.length - 1].seq);
    if (sel.value !== newest && want.some((f) => String(f.seq) === newest))
      sel.value = newest;
  }

  if (!frames.length) {
    tbody.innerHTML = '<tr><td colspan="11" class="meta">尚无帧。</td></tr>';
    return;
  }
  const rows = frames.slice(-160).reverse();
  tbody.innerHTML = rows
    .map((f) => {
      const seqShown = f.status === "submitted" || f.status === "pending" ? "—" : f.markerDecoded;
      return `<tr>
        <td>${f.seq}</td>
        <td class="st-${esc(f.status)}">${esc(f.status)}${f.corrupted ? " ⚠" : ""}</td>
        <td>${f.surfaceIndex >= 0 ? f.surfaceIndex : "—"}</td>
        <td>${f.bitstreamReady ? f.auBytes + " B" + (f.keyframe ? " · I" : "") : "—"}</td>
        <td>${f.markerExpected} / ${seqShown}</td>
        <td>${flag(f.patchesOk)}</td>
        <td>${flag(f.colorsOk)}</td>
        <td>${f.psnrMae >= 0 ? f.psnrMae.toFixed(2) : "—"}</td>
        <td>${fmtMs(f.submitToAuMs)}</td>
        <td>${fmtMs(f.submitToReleaseMs)}</td>
        <td title="${esc(f.note)}">${esc((f.note || "").slice(0, 46))}</td>
      </tr>`;
    })
    .join("");
}

function flag(v) {
  return v ? '<span style="color:#7ee2a1">✓</span>' : '<span style="color:#ff9f9a">✗</span>';
}

function renderMetrics(snap) {
  const m = snap.metrics || {};
  const cap = snap.caps || {};
  const sw = snap.config.backend === "openh264";
  const cells = [
    ["表面池", snap.surfaces.length + " 个", "", "有限池，" + fmtBytes(snap.mappedBytes)],
    ["最大在途", m.maxInFlight ?? 0, "", "受池大小约束"],
    ["背压等待", m.backpressureStalls || 0, m.backpressureStalls ? "warn" : "", "累计 " + fmtMs(m.stallMsTotal)],
    ["提交→码流 p50", fmtMs(m.p50SubmitToAuMs), "", "最大 " + fmtMs(m.maxSubmitToAuMs)],
    ["提交→可复用 p50", fmtMs(m.p50SubmitToReleaseMs), "", "最大 " + fmtMs(m.maxSubmitToReleaseMs)],
    ["码流 / 提交", (m.bitstreams || 0) + " / " + (m.submitted || 0), "", "每帧一个访问单元"],
    ["已归还表面", m.released || 0, "", "DQBUF/延迟回执计数"],
    ["未归还", snap.leakedSurfaces || 0, snap.leakedSurfaces ? "bad" : "good", "结束时应为 0"],
    ["乱序 / 重复", (m.outOfOrder || 0) + " / " + (m.duplicateSeq || 0), m.outOfOrder ? "bad" : "good", "按帧号回映"],
    ["解码错误", m.decodeErrors || 0, m.decodeErrors ? "bad" : "good", "独立解码器"],
    ["帧号不符", m.markerMismatches || 0, m.markerMismatches ? "warn" : "good", "标记块核对"],
    ["画面不符", m.pictureMismatches || 0, m.pictureMismatches ? "bad" : "good", "MAE 阈值 12"],
    ["RSS 基线 / 峰值", (m.baselineRssKb || 0) + " / " + (m.peakRssKb || 0) + " KB", "", "进程常驻内存"],
    ["EOS 排空", snap.eosComplete ? "完成" : snap.eosRequested ? "进行中" : "—", snap.eosComplete ? "good" : "", "共 " + (snap.framesDrained || 0) + " 帧"],
    ["故障检出", (m.detected || 0) + " 帧", "", "注入故障模式下应恰好 1 帧"],
  ];
  $("metricGrid").innerHTML = cells
    .map(([label, value, cls, hint]) => `<div class="metric ${cls}">
      <div class="mv">${esc(value)}</div>
      <div class="ml">${esc(label)}</div>
      <div class="mh">${esc(hint)}</div>
    </div>`)
    .join("");
}

function renderVerdict(snap) {
  const created = snap.sessionCreated;
  const vp = snap.pipelineVerified;
  $("vCreated").textContent = "会话创建：" + (created ? "成功" : "—");
  const pipe = $("vPipeline");
  const hw = $("vHardware");
  if (snap.status === "running" || snap.status === "draining" || snap.status === "starting") {
    pipe.textContent = "链路：运行中";
    pipe.className = "tag warn";
  } else if (vp) {
    pipe.textContent = "链路：已证明";
    pipe.className = "tag ok";
  } else if (snap.status === "completed") {
    pipe.textContent = "链路：未通过验收";
    pipe.className = "tag bad";
  } else {
    pipe.textContent = "链路：—";
    pipe.className = "tag";
  }
  if (snap.hardwareCovered) {
    hw.textContent = "硬件覆盖：真实设备";
    hw.className = "tag ok";
  } else {
    hw.textContent = "硬件覆盖：未覆盖";
    hw.className = "tag warn";
  }
  $("vNote").textContent = snap.verificationNote || snap.error || "尚未运行。";
}

function renderLog(snap) {
  const el = $("log");
  const nearBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 24;
  const lines = snap.log || [];
  el.textContent = lines.join("\n");
  if (nearBottom) el.scrollTop = el.scrollHeight;
}

function frameInfo(snap, seq) {
  const f = (snap.frames || []).find((x) => x.seq === seq);
  if (!f) return "";
  return [
    `<span>帧号 <b>${f.seq}</b></span>`,
    `<span>状态 <b>${esc(f.status)}</b></span>`,
    `<span>标记 期望/实际 <b>${f.markerExpected} / ${f.markerDecoded}</b></span>`,
    `<span>色块 <b>${f.patchesOk ? "✓" : "✗"}</b></span>`,
    `<span>颜色 <b>${f.colorsOk ? "✓" : "✗"}</b></span>`,
    `<span>MAE <b>${f.psnrMae >= 0 ? f.psnrMae.toFixed(2) : "—"}</b></span>`,
    `<span>码流 <b>${f.bitstreamReady ? f.auBytes + " B" : "—"}</b></span>`,
    `<span>解码尺寸 <b>${f.decodedW ? f.decodedW + "×" + f.decodedH : "—"}</b></span>`,
    `<span>提交→码流 <b>${fmtMs(f.submitToAuMs)}</b></span>`,
    `<span>提交→可复用 <b>${fmtMs(f.submitToReleaseMs)}</b></span>`,
    f.note ? `<span>说明 <b>${esc(f.note)}</b></span>` : "",
  ].join("");
}

function pickSeq(snap) {
  if (!state.follow && state.selectedSeq !== null) return state.selectedSeq;
  const frames = (snap.frames || []).filter((f) => f.status !== "pending");
  if (!frames.length) return null;
  return frames[frames.length - 1].seq;
}

function refreshImages(snap) {
  const seq = pickSeq(snap);
  if (seq === null) return;
  const frames = snap.frames || [];
  const f = frames.find((x) => x.seq === seq);
  const srcOk = f && f.hasSrc;
  const outOk = f && f.hasOut;
  const setImg = (id, kind, ok) => {
    const img = $(id);
    if (!ok) { img.removeAttribute("src"); return; }
    const key = seq + ":" + (f ? f.status + ":" + f.auBytes : "") + ":" + kind;
    const url = `/api/frame.png?kind=${kind}&seq=${seq}&w=360&t=${encodeURIComponent(key)}`;
    if (img.dataset.key !== key) {
      img.dataset.key = key;
      img.src = url;
    }
  };
  setImg("imgSrc", "src", srcOk);
  setImg("imgOut", "out", outOk);
  $("srcMeta").textContent = srcOk
    ? `帧 ${seq} · ${snap.format.visibleW}×${snap.format.visibleH}`
    : "—";
  $("outMeta").textContent = outOk
    ? `帧 ${seq} · 解码 ${f.decodedW}×${f.decodedH} · MAE ${f.psnrMae >= 0 ? f.psnrMae.toFixed(2) : "—"}`
    : "—";
  $("frameInfo").innerHTML = frameInfo(snap, seq);
}

function applySnapshot(snap) {
  state.latest = snap;
  const [stText, stCls] = stateChip(snap.status);
  statusChip($("chipStatus"), stText, stCls);
  statusChip($("chipBackend"), snap.caps && snap.caps.displayName ? truncate(snap.caps.displayName, 38) : "后端 —", "chip-muted");
  statusChip($("chipCoverage"), snap.hardwareCovered ? "真实硬件" : "硬件未覆盖", snap.hardwareCovered ? "chip-good" : "chip-warn");

  const active = snap.status === "running" || snap.status === "draining" || snap.status === "starting" || snap.status === "completed";
  setRunning(snap.status === "running" || snap.status === "draining" || snap.status === "starting");

  renderVerdict(snap);
  renderSurfaces(snap);
  renderFrames(snap);
  renderMetrics(snap);
  renderLog(snap);
  refreshImages(snap);
}

function truncate(s, n) {
  return s.length > n ? s.slice(0, n - 1) + "…" : s;
}

let pollInFlight = false;
async function pollNow() {
  if (pollInFlight) return;
  pollInFlight = true;
  try {
    const snap = await api("/api/state");
    applySnapshot(snap);
  } catch (e) {
    statusChip($("chipStatus"), "服务中断", "chip-bad");
  } finally {
    pollInFlight = false;
  }
}

function loop() {
  pollNow();
  const s = state.latest;
  const fast = !s || s.status === "running" || s.status === "draining" || s.status === "starting";
  state.pollTimer = setTimeout(loop, fast ? 250 : 1200);
}

function wire() {
  $("btnStart").addEventListener("click", start);
  $("btnStop").addEventListener("click", stop);
  $("fBackend").addEventListener("change", selectedBackendHint);
  $("fMode").addEventListener("change", () => {
    $("fLag").disabled = $("fMode").value !== "late";
    $("modeHint").textContent = MODE_HINTS[$("fMode").value] || "";
  });
  $("btnLive").addEventListener("click", () => {
    state.follow = !state.follow;
    $("btnLive").setAttribute("aria-pressed", String(state.follow));
    if (!state.follow) {
      const v = $("frameSel").value;
      if (v) state.selectedSeq = Number(v);
    } else {
      pollNow();
    }
  });
  $("frameSel").addEventListener("change", () => {
    state.selectedSeq = Number($("frameSel").value);
    state.follow = false;
    $("btnLive").setAttribute("aria-pressed", "false");
    if (state.latest) refreshImages(state.latest);
  });
  $("modeHint").textContent = MODE_HINTS[$("fMode").value];
}

wire();
loadDevices();
loop();
