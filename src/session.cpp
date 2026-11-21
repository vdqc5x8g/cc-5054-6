#include "session.h"

#include <cstring>
#include <unordered_map>

#include "png.h"
#include "scheduler.h"

namespace {

double percentile(std::vector<double>& v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  size_t i = (size_t)std::min<double>((double)v.size() - 1,
                                      p * (double)(v.size() - 1));
  return v[i];
}

uint32_t crc32of(const std::vector<uint8_t>& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d) {
    c ^= b;
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
  }
  return ~c;
}

char kindOf(const EncoderEvent& ev) {
  switch (ev.type) {
    case EncoderEvent::Type::BitstreamReady: return 'p';
    case EncoderEvent::Type::SurfaceReleased: return 'r';
    case EncoderEvent::Type::Fatal: return 'f';
  }
  return '?';
}

}  // namespace

Session::Session() = default;

Session::~Session() {
  stop();
  join(8000);
  if (worker_.joinable()) worker_.detach();
}

bool Session::start(const SessionConfig& cfg, std::string& err) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (worker_.joinable() && !workerDone_ && snap_.status != "completed" &&
        snap_.status != "failed" && snap_.status != "stopped") {
      err = "a run is already in progress";
      return false;
    }
  }
  if (worker_.joinable() && !join(5000)) {
    err = "previous run is still shutting down";
    return false;
  }
  SessionConfig c = cfg;
  if (c.width < 64 || c.width > 1920 || c.height < 64 || c.height > 1080) {
    err = "size must be within 64..1920 x 64..1080";
    return false;
  }
  if (c.width % 2 || c.height % 2) {
    err = "visible width/height must be even for 4:2:0 encoding";
    return false;
  }
  if (c.fps < 1 || c.fps > 60) {
    err = "fps must be 1..60";
    return false;
  }
  if (c.bitrate < 50000 || c.bitrate > 50000000) {
    err = "bitrate must be 50k..50M";
    return false;
  }
  if (c.gop < 1 || c.gop > 600) {
    err = "gop must be 1..600";
    return false;
  }
  if (c.pool < 2 || c.pool > 8) {
    err = "pool must be 2..8";
    return false;
  }
  if (c.frames < 1 || c.frames > 500) {
    err = "frames must be 1..500";
    return false;
  }
  if (c.mode != "paced" && c.mode != "burst" && c.mode != "late" &&
      c.mode != "guard" && c.mode != "corrupt" && c.mode != "earlyreuse") {
    err = "unknown send mode";
    return false;
  }
  if (c.receiptLagMs < 0 || c.receiptLagMs > 2000) {
    err = "receiptLag must be 0..2000 ms";
    return false;
  }
  if (c.readDelayMs < 0 || c.readDelayMs > 2000 || c.scanMs < 0 ||
      c.scanMs > 2000) {
    err = "readDelay/scan must be 0..2000 ms";
    return false;
  }
  if (c.backend != "openh264" && c.backend != "v4l2") {
    err = "unknown backend";
    return false;
  }
  if (c.sched != "fixed" && c.sched != "elastic") {
    err = "unknown surface scheduler";
    return false;
  }
  if (c.poolMin < 2 || c.poolMin > 8 || c.poolMax < 2 || c.poolMax > 8) {
    err = "poolMin/poolMax must be 2..8";
    return false;
  }
  if (c.sched == "elastic" && !(c.poolMin <= c.pool && c.pool <= c.poolMax)) {
    err = "elastic scheduling needs poolMin <= pool <= poolMax";
    return false;
  }
  if (c.order != "any" && c.order != "packet" && c.order != "return") {
    err = "order must be any|packet|return";
    return false;
  }
  for (char ch : c.script) {
    if (ch != 'p' && ch != 'r') {
      err = "script may only contain 'p' (packet) and 'r' (return)";
      return false;
    }
  }
  if (c.script.size() > 4096) {
    err = "script too long";
    return false;
  }
  if (c.switchAt >= 0) {
    if (c.switchAt == 0 || c.switchAt >= c.frames) {
      err = "switchAt must be within 1..frames-1";
      return false;
    }
    if (c.switchWidth < 64 || c.switchWidth > 1920 || c.switchHeight < 64 ||
        c.switchHeight > 1080 || c.switchWidth % 2 || c.switchHeight % 2) {
      err = "switch resolution must be even and within 64..1920 x 64..1080";
      return false;
    }
  }
  if (c.mode == "earlyreuse" && c.frames < 2) {
    err = "earlyreuse needs at least 2 frames";
    return false;
  }

  // Harness defaults that make the intended interleavings exist.
  if (c.mode == "late" && c.receiptLagMs == 0) c.receiptLagMs = 120;
  if (c.mode == "earlyreuse") {
    if (c.receiptLagMs == 0) c.receiptLagMs = 120;
    if (c.readDelayMs == 0) c.readDelayMs = 40;
    if (c.scanMs == 0) c.scanMs = 60;
  }
  if (c.switchAt >= 0 && c.receiptLagMs == 0) c.receiptLagMs = 120;
  if (c.receiptLagMs > 0 && c.readDelayMs > c.receiptLagMs)
    c.readDelayMs = c.receiptLagMs;

  SessionSnapshot fresh;
  fresh.status = "starting";
  fresh.cfg = c;
  fresh.phase = "initialising";
  fresh.startedAtUs = nowUs();
  fresh.stepMode = c.step;

  std::lock_guard<std::mutex> lk(mu_);
  cfg_ = c;
  snap_ = std::move(fresh);
  frames_.clear();
  finalSurfaces_.clear();
  artifacts_.clear();
  artifactOrder_.clear();
  submitUs_.clear();
  gens_.clear();
  m_ = RunMetrics();
  maxInFlightObserved_ = 0;
  latAu_.clear();
  latRel_.clear();
  relLagMs_.clear();
  fatal_.clear();
  pending_.clear();
  tokens_.clear();
  trace_.clear();
  scriptPos_ = 0;
  stepMode_ = c.step;
  gateBlockedSince_ = -1;
  overwritePending_ = false;
  stopFlag_ = false;
  workerDone_ = false;
  runStartUs_ = nowUs();
  m_.baselineRssKb = (int)processRssKb();
  m_.peakRssKb = m_.baselineRssKb;
  worker_ = std::thread([this] {
    workerMain();
    workerDone_ = true;
  });
  return true;
}

void Session::stop() { stopFlag_ = true; }

bool Session::join(int timeoutMs) {
  int waited = 0;
  while (!workerDone_) {
    if (waited >= timeoutMs) return false;
    sleepUs(5000);
    waited += 5;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (worker_.joinable()) worker_.join();
  return true;
}

bool Session::stepEvents(const std::string& kind, int n, std::string& err) {
  char k = 0;
  if (kind == "packet") k = 'p';
  else if (kind == "return") k = 'r';
  else if (kind == "any") k = 'a';
  else {
    err = "kind must be packet|return|any";
    return false;
  }
  if (n < 1 || n > 64) {
    err = "n must be 1..64";
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  if (!stepMode_) {
    err = "run is not in step mode";
    return false;
  }
  if (workerDone_) {
    err = "no run is active";
    return false;
  }
  for (int i = 0; i < n; i++) tokens_.push_back(k);
  return true;
}

void Session::continueRun() {
  std::lock_guard<std::mutex> lk(mu_);
  stepMode_ = false;
  tokens_.clear();
  if (scriptPos_ < cfg_.script.size()) {
    scriptPos_ = cfg_.script.size();
    logLine("手动继续：放弃剩余调度脚本，事件自由放行");
  } else {
    logLine("手动继续：退出单步，事件自由放行");
  }
  snap_.stepMode = false;
}

void Session::logLine(const std::string& s) {
  snap_.log.push_back(s);
  if (snap_.log.size() > 300) snap_.log.erase(snap_.log.begin());
}

FrameView* Session::frameFor(uint64_t seq) {
  if (seq < frames_.size()) return &frames_[seq];
  return nullptr;
}

// ---------------------------------------------------------------------------
// Generations
// ---------------------------------------------------------------------------

bool Session::makeGeneration(int width, int height, std::string& err) {
  auto g = std::unique_ptr<Generation>(new Generation());
  g->id = (int)gens_.size();
  g->width = width;
  g->height = height;

  EncoderSettings es;
  es.width = width;
  es.height = height;
  es.fps = cfg_.fps;
  es.bitrate = cfg_.bitrate;
  es.gop = cfg_.gop;
  es.pool = cfg_.pool;
  es.poolMax = cfg_.sched == "elastic" ? cfg_.poolMax : 0;
  es.format = cfg_.format;
  es.device = cfg_.device;
  bool sw = cfg_.backend != "v4l2";
  es.receiptLagMs = sw ? cfg_.receiptLagMs : 0;
  es.readDelayMs = sw ? cfg_.readDelayMs : 0;
  es.scanMs = sw ? cfg_.scanMs : 0;

  if (cfg_.backend == "v4l2")
    g->enc = makeV4l2Encoder(es, err);
  else
    g->enc = makeOpenH264Encoder(es, err);
  if (!g->enc) {
    err = "后端构造失败：" + err;
    return false;
  }
  if (!g->enc->createPool(cfg_.pool, err)) {
    err = "无法分配所需表面：" + err;
    return false;
  }
  g->caps = g->enc->caps();
  g->format = g->enc->format();
  g->layout.fmt = g->format.fmt;
  g->layout.visibleW = g->format.visibleW;
  g->layout.visibleH = g->format.visibleH;
  g->layout.codedW = g->format.codedW;
  g->layout.codedH = g->format.codedH;
  g->layout.yStride = g->format.stride0;
  g->layout.uvStride = g->format.stride1;
  g->layout.uvRows = g->format.codedH / 2;
  g->idleSinceUs.assign((size_t)g->enc->surfaceCount(), nowUs());

  {
    std::lock_guard<std::mutex> lk(mu_);
    logLine("代次 " + std::to_string(g->id) + "：编码会话已创建（" +
            g->caps.displayName + "，" + std::to_string(width) + "x" +
            std::to_string(height) + "）");
    for (int i = 0; i < g->enc->surfaceCount(); i++) {
      Surface& s = g->enc->surface(i);
      logLine("代次 " + std::to_string(g->id) + " 表面 " + std::to_string(i) +
              "：" + pixFmtName(s.fmt) + " 可视 " + std::to_string(s.visibleW) +
              "x" + std::to_string(s.visibleH) + " 编码区 " +
              std::to_string(s.codedW) + "x" + std::to_string(s.codedH) +
              " 步幅 " + std::to_string(s.refs.stride[0]) + "/" +
              std::to_string(s.refs.stride[1]) + "/" +
              std::to_string(s.refs.stride[2]) +
              (s.deviceMemory ? " 设备映射" : " 主机内存") + "，" +
              std::to_string(s.totalBytes) + " 字节");
    }
  }

  if (cfg_.verify) {
    std::string derr;
    if (!g->dec.open(derr)) {
      std::lock_guard<std::mutex> lk(mu_);
      logLine("代次 " + std::to_string(g->id) +
              " 解码器打开失败（" + derr + "），其帧无法做画面核对");
    } else {
      g->decoderOpen = true;
    }
  }

  bool first = gens_.empty();
  gens_.push_back(std::move(g));
  if (first) {
    std::lock_guard<std::mutex> lk(mu_);
    Generation& cur = *gens_.back();
    snap_.caps = cur.caps;
    snap_.format = cur.format;
    snap_.sessionCreated = true;
    snap_.hardwareCovered = cur.caps.hardware;
    snap_.timingSource = cur.caps.hardware ? "device" : "simulated";
    snap_.verifyEnabled = cfg_.verify;
    snap_.decoderOpen = cur.decoderOpen;
    snap_.status = "running";
    snap_.phase = "feeding frames";
    logLine("会话创建并不等于送帧链路正确；下面分别检查提交、异步码流回执、解码核对与表面归还。");
    logLine(std::string("表面调度器：") +
            (cfg_.sched == "elastic"
                 ? "弹性池（按归还反馈扩缩），初始 " + std::to_string(cfg_.pool) +
                       "，下限 " + std::to_string(cfg_.poolMin) + "，上限 " +
                       std::to_string(cfg_.poolMax)
                 : "固定池，" + std::to_string(cfg_.pool) + " 个表面"));
    if (cfg_.readDelayMs > 0 || cfg_.scanMs > 0)
      logLine("延迟读取模型：提交后 " + std::to_string(cfg_.readDelayMs) +
              " ms 开始扫描输入表面，扫描 " + std::to_string(cfg_.scanMs) +
              " ms；扫描完成产包，" + std::to_string(cfg_.receiptLagMs) +
              " ms 后才允许回收——构造“包已可读、表面未获准回收”的交错");
    if (!cfg_.script.empty())
      logLine("调度脚本重放：" + std::to_string(cfg_.script.size()) +
              " 个事件（p=产包 r=归还），顺序由脚本强制");
    else if (cfg_.order != "any")
      logLine(std::string("事件顺序偏好：") +
              (cfg_.order == "packet" ? "产包事件优先" : "归还事件优先"));
    if (stepMode_)
      logLine("单步模式：产包与归还事件需经界面逐个放行");
  } else {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.decoderOpen = gens_.back()->decoderOpen;
  }
  return true;
}

void Session::switchGeneration() {
  Generation& old = cur();
  {
    std::string eerr;
    old.enc->requestEos(eerr);
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    old.eosRequested = true;
    logLine("分辨率切换：帧 " + std::to_string(cfg_.switchAt) + " 起改用 " +
            std::to_string(cfg_.switchWidth) + "x" +
            std::to_string(cfg_.switchHeight) +
            "；旧代次 " + std::to_string(old.id) +
            " 的资源仍在排空，归还按代次记账");
  }
  std::string err;
  if (!makeGeneration(cfg_.switchWidth, cfg_.switchHeight, err)) {
    std::lock_guard<std::mutex> lk(mu_);
    finishRun("failed", "新分辨率建池失败：" + err);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    logLine("新代次 " + std::to_string(gens_.back()->id) +
            " 已建池并开始送帧；旧代次表面在实际确认归还前不会释放");
    updatePeaks();
  }
}

void Session::closeGeneration(Generation& g) {
  if (g.closed) return;
  if (g.enc) g.enc->shutdown();
  g.enc.reset();
  g.dec.close();
  g.decoderOpen = false;
  g.closed = true;
}

// ---------------------------------------------------------------------------
// Event pump and gate
// ---------------------------------------------------------------------------

void Session::pumpGeneration(Generation& g) {
  if (g.closed || !g.enc) return;
  std::vector<EncoderEvent> evs;
  std::string perr;
  int n = g.enc->poll(evs, 2, perr);
  if (n < 0) {
    std::lock_guard<std::mutex> lk(mu_);
    fatal_ = perr;
    logLine("后端轮询失败：" + perr);
    return;
  }
  for (auto& ev : evs) {
    if (ev.type == EncoderEvent::Type::Fatal) {
      std::lock_guard<std::mutex> lk(mu_);
      fatal_ = ev.detail;
      logLine("后端致命错误：" + ev.detail);
      continue;
    }
    PendingEv pe;
    pe.ev = std::move(ev);
    pe.gen = g.id;
    pending_.push_back(std::move(pe));
  }
}

bool Session::pumpAndGate() {
  for (auto& g : gens_) pumpGeneration(*g);
  std::lock_guard<std::mutex> lk(mu_);
  int applied = applyGate();
  updatePeaks();
  return applied > 0;
}

bool Session::quiescent() const {
  // No pending events, nothing in flight in any generation, and every
  // generation has drained: the event the gate is waiting for cannot come.
  if (!pending_.empty()) return false;
  for (auto& gp : gens_) {
    if (gp->closed) continue;
    for (int i = 0; i < gp->enc->surfaceCount(); i++) {
      Surface::State st = gp->enc->surface(i).state;
      if (st == Surface::State::Queued || st == Surface::State::Filling)
        return false;
    }
    if (gp->eosRequested && !gp->enc->eosComplete()) return false;
  }
  return true;
}

int Session::applyGate() {
  // mu_ held by caller.
  int applied = 0;
  while (!pending_.empty()) {
    char want = 0;
    bool scripted = scriptPos_ < cfg_.script.size();
    bool strict = false;
    if (scripted) {
      want = cfg_.script[scriptPos_];
      strict = true;
    } else if (stepMode_) {
      if (tokens_.empty()) break;
      want = tokens_.front();
      strict = true;
    } else if (cfg_.order == "packet") {
      want = 'p';
    } else if (cfg_.order == "return") {
      want = 'r';
    }
    auto it = pending_.begin();
    if (want == 'p' || want == 'r') {
      it = std::find_if(pending_.begin(), pending_.end(),
                        [&](const PendingEv& pe) {
                          return kindOf(pe.ev) == want;
                        });
      if (it == pending_.end()) {
        if (strict) {
          // The wanted kind is not pending. If it can never arrive, stop
          // waiting instead of deadlocking the schedule replay.
          if (quiescent()) {
            if (scripted) {
              logLine("调度脚本等待的事件已不会出现，提前结束脚本（已放行 " +
                      std::to_string(scriptPos_) + "/" +
                      std::to_string(cfg_.script.size()) + "）");
              scriptPos_ = cfg_.script.size();
            }
            tokens_.clear();
            break;
          }
          if (gateBlockedSince_ < 0) gateBlockedSince_ = nowUs();
          break;
        }
        it = pending_.begin();  // preference only: fall back to arrival order
      }
    }
    gateBlockedSince_ = -1;
    PendingEv pe = std::move(*it);
    pending_.erase(it);
    applyEvent(pe);
    trace_ += kindOf(pe.ev);
    if (scripted)
      scriptPos_++;
    else if (stepMode_ && !tokens_.empty())
      tokens_.pop_front();
    applied++;
  }
  // Publish gate view.
  int pk = 0, rt = 0;
  for (auto& pe : pending_)
    if (kindOf(pe.ev) == 'p')
      pk++;
    else
      rt++;
  snap_.pendingPackets = pk;
  snap_.pendingReturns = rt;
  snap_.stepMode = stepMode_;
  snap_.scriptPos = scriptPos_;
  snap_.scriptLen = cfg_.script.size();
  snap_.trace = trace_;
  return applied;
}

void Session::applyEvent(PendingEv& pe) {
  Generation* g = nullptr;
  for (auto& gp : gens_)
    if (gp->id == pe.gen) {
      g = gp.get();
      break;
    }
  if (!g || g->closed) {
    logLine("代次 " + std::to_string(pe.gen) +
            " 已销毁后才到达的事件被丢弃（这不应发生）");
    return;
  }
  int64_t now = nowUs();
  if (pe.ev.type == EncoderEvent::Type::BitstreamReady)
    handleBitstream(pe.ev, *g, now);
  else if (pe.ev.type == EncoderEvent::Type::SurfaceReleased)
    handleRelease(pe.ev, *g, now);
}

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

namespace {
int countState(IEncoder* enc, Surface::State want) {
  int n = 0;
  for (int i = 0; i < enc->surfaceCount(); i++)
    if (enc->surface(i).state == want) n++;
  return n;
}
int activeSurfaces(IEncoder* enc) {
  int n = 0;
  for (int i = 0; i < enc->surfaceCount(); i++)
    if (enc->surface(i).state != Surface::State::Retired) n++;
  return n;
}
}  // namespace

double Session::expectedOldestWaitMsLocked(Generation& g) {
  int64_t oldest = -1;
  for (int i = 0; i < g.enc->surfaceCount(); i++) {
    Surface& s = g.enc->surface(i);
    if (s.state != Surface::State::Queued || s.heldSeq == UINT64_MAX) continue;
    auto it = submitUs_.find(s.heldSeq);
    if (it != submitUs_.end())
      oldest = oldest < 0 ? it->second : std::min(oldest, it->second);
  }
  if (oldest < 0) return -1;
  double lagMs;
  if (!relLagMs_.empty()) {
    std::vector<double> copy = relLagMs_;
    lagMs = percentile(copy, 0.5);
  } else if (cfg_.receiptLagMs > 0) {
    lagMs = (double)cfg_.receiptLagMs;
  } else {
    lagMs = 1000.0 / std::max(1, cfg_.fps);
  }
  double ageMs = (double)(nowUs() - oldest) / 1000.0;
  return std::max(0.0, lagMs - ageMs);
}

bool Session::acquireSurface(Generation& g, int& idx, int64_t) {
  idx = -1;
  int64_t stallStartUs = nowUs();
  bool loggedWait = false;
  bool loggedGrowFail = false;
  const bool elastic = cfg_.sched == "elastic";
  for (;;) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (int k = 0; k < g.enc->surfaceCount(); k++) {
        if (g.enc->surface(k).state == Surface::State::Free) {
          idx = k;
          g.idleSinceUs[k] = 0;
          return true;
        }
      }
      if (elastic) {
        SchedInput in;
        in.active = activeSurfaces(g.enc.get());
        in.poolMin = cfg_.poolMin;
        in.poolMax = cfg_.poolMax;
        in.freeCount = 0;
        in.inFlight = countState(g.enc.get(), Surface::State::Queued);
        in.framePeriodMs = 1000.0 / std::max(1, cfg_.fps);
        in.oldestWaitMs = expectedOldestWaitMsLocked(g);
        SchedDecision d = decideAcquire(in, true);
        if (d.act == SchedAct::Grow) {
          std::string gerr;
          if (g.enc->growPool(gerr)) {
            int ni = g.enc->surfaceCount() - 1;
            g.idleSinceUs.push_back(0);
            m_.grows++;
            logLine("弹性增长 → " + std::to_string(activeSurfaces(g.enc.get())) +
                    " 个活跃表面：" + d.why);
            idx = ni;
            updatePeaks();
            return true;
          }
          if (!loggedGrowFail) {
            logLine("增长被后端拒绝（" + gerr + "），改为等待归还");
            loggedGrowFail = true;
          }
          m_.schedWaits++;
        } else {
          m_.schedWaits++;
          if (!loggedWait) {
            logLine("弹性等待：" + d.why);
            loggedWait = true;
          }
        }
      }
    }
    if (stopFlag_.load()) return false;
    // The earlyreuse overwrite is wall-clock armed; a starved acquire loop is
    // exactly where the worker sits while the victim's scan window passes.
    maybeInjectOverwrite(nowUs());
    pumpAndGate();
    if (!fatal_.empty()) return false;
    if (!stepMode_ && cfg_.script.empty() &&
        nowUs() - stallStartUs > 15000000) {
      std::lock_guard<std::mutex> lk(mu_);
      finishRun("failed",
                "15 秒内没有任何表面归还（池已耗尽且回执停止）");
      return false;
    }
  }
}

void Session::maybeShrink(Generation& g) {
  // mu_ held by caller.
  if (cfg_.sched != "elastic") return;
  int active = activeSurfaces(g.enc.get());
  if (active <= cfg_.poolMin) return;
  int freeCount = countState(g.enc.get(), Surface::State::Free);
  if (freeCount < 2) return;
  int64_t now = nowUs();
  int oldestIdx = -1;
  int64_t oldestIdle = 0;
  for (int i = 0; i < g.enc->surfaceCount(); i++) {
    if (g.enc->surface(i).state != Surface::State::Free) continue;
    int64_t since = g.idleSinceUs[i];
    if (since <= 0) continue;
    int64_t idle = now - since;
    if (idle > oldestIdle) {
      oldestIdle = idle;
      oldestIdx = i;
    }
  }
  if (oldestIdx < 0) return;
  SchedInput in;
  in.active = active;
  in.poolMin = cfg_.poolMin;
  in.poolMax = cfg_.poolMax;
  in.freeCount = freeCount;
  in.framePeriodMs = 1000.0 / std::max(1, cfg_.fps);
  in.idleUs = oldestIdle;
  SchedDecision d = decideRelease(in, true);
  if (d.act != SchedAct::Shrink) return;
  std::string serr;
  if (g.enc->shrinkPool(oldestIdx, serr)) {
    m_.shrinks++;
    logLine("弹性收缩 → " + std::to_string(activeSurfaces(g.enc.get()) - 1) +
            " 个活跃表面：表面 " + std::to_string(oldestIdx) + " 已退役（" +
            d.why + "）");
    updatePeaks();
  } else {
    logLine("收缩表面 " + std::to_string(oldestIdx) +
            " 被后端拒绝：" + serr);
  }
}

// ---------------------------------------------------------------------------
// earlyreuse fault injection
// ---------------------------------------------------------------------------

void Session::maybeInjectOverwrite(int64_t now) {
  if (!overwritePending_ || now < overwriteAtUs_) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (!overwritePending_) return;
  overwritePending_ = false;
  if (overwriteGen_ < 0 || overwriteGen_ >= (int)gens_.size()) return;
  Generation& g = *gens_[overwriteGen_];
  if (g.closed || overwriteSurface_ < 0 ||
      overwriteSurface_ >= g.enc->surfaceCount())
    return;
  Surface& s = g.enc->surface(overwriteSurface_);
  uint64_t victimSeq = overwritePlanSeq_ - 1;
  if (s.state != Surface::State::Queued || s.heldSeq != victimSeq) {
    logLine("故障注入窗口错过：表面 " + std::to_string(overwriteSurface_) +
            " 已提前归还，本次无法构造提前复用（运行将按未检出判失败）");
    return;
  }
  // The ownership violation itself: the backend is still scanning this
  // surface for frame victimSeq, and we overwrite it with the next frame.
  m_.ownershipViolations++;
  FramePlan plan =
      renderFrame(overwritePlanSeq_, g.width, g.height, false);
  convertRgbToSurface(plan, g.layout, s.refs, s.planeBytes);
  logLine("故障注入：表面 " + std::to_string(overwriteSurface_) +
          " 在编码器仍持有（帧 " + std::to_string(victimSeq) +
          " 读取进行中）时被覆写为帧 " + std::to_string(overwritePlanSeq_) +
          " 的内容；若所有权被尊重，解码画面应出现可识别的串帧");
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void Session::workerMain() {
  std::string err;

  {
    std::lock_guard<std::mutex> lk(mu_);
    frames_.assign((size_t)cfg_.frames, FrameView());
    for (int i = 0; i < cfg_.frames; i++) {
      frames_[i].seq = (uint64_t)i;
      frames_[i].order = i;
    }
    logLine("开始运行：后端=" + cfg_.backend + "，送帧方式=" + cfg_.mode +
            "，调度器=" + cfg_.sched + "，表面池=" + std::to_string(cfg_.pool) +
            "，帧数=" + std::to_string(cfg_.frames));
  }

  if (!makeGeneration(cfg_.width, cfg_.height, err)) {
    std::lock_guard<std::mutex> lk(mu_);
    finishRun("failed", err);
    return;
  }

  const int64_t framePeriodUs = 1000000 / std::max(1, cfg_.fps);
  const bool paced = cfg_.mode == "paced" || cfg_.mode == "late" ||
                     cfg_.mode == "guard" || cfg_.mode == "corrupt" ||
                     cfg_.mode == "earlyreuse";
  int64_t nextDeadlineUs = runStartUs_;
  const int injectedSeq =
      cfg_.mode == "corrupt" ? std::min(2, cfg_.frames - 1) : -1;
  const int victimSeq =
      cfg_.mode == "earlyreuse" ? std::min(2, cfg_.frames - 2) : -1;

  for (int i = 0; i < cfg_.frames; i++) {
    if (stopFlag_.load() && i > 0) break;
    if (!fatal_.empty()) break;

    if (i == cfg_.switchAt) {
      switchGeneration();
      if (!fatal_.empty() || gens_.empty()) break;
      // finishRun may have been called by a failed switch.
      std::lock_guard<std::mutex> lk(mu_);
      if (snap_.status == "failed") return;
    }

    Generation& g = cur();

    if (paced) {
      while (true) {
        maybeInjectOverwrite(nowUs());
        int64_t wait = nextDeadlineUs - nowUs();
        if (wait <= 0) break;
        pumpAndGate();
        if (!fatal_.empty()) break;
        if (stopFlag_.load()) break;
        if (wait > 3000) sleepUs(1000);
      }
      if (stopFlag_.load() && i > 0) break;
      if (!fatal_.empty()) break;
    }

    // Pick a FREE surface, grow the pool, or wait for a release.
    int idx = -1;
    int64_t stallStartUs = nowUs();
    if (!acquireSurface(g, idx, nextDeadlineUs)) {
      std::lock_guard<std::mutex> lk(mu_);
      if (snap_.status == "failed") return;  // finishRun already ran
      if (!fatal_.empty()) {
        finishRun("failed", fatal_);
        return;
      }
    }
    if (idx < 0) break;  // stopped: fall through to the drain phase
    int64_t stallUs = nowUs() - stallStartUs;
    if (stallUs > 1500) {
      std::lock_guard<std::mutex> lk(mu_);
      m_.backpressureStalls++;
      m_.stallMsTotal += (double)stallUs / 1000.0;
      if (m_.backpressureStalls <= 6)
        logLine("背压：帧 " + std::to_string(i) + " 等待 " +
                std::to_string(stallUs / 1000) + " ms 才拿到空闲表面；" +
                std::to_string(activeSurfaces(g.enc.get())) +
                " 个活跃表面都仍被编码器持有");
    }

    // In guard mode, verify the ownership semantics the hard way: a surface
    // still held by the codec must be reported non-writable.
    if (cfg_.mode == "guard") {
      std::lock_guard<std::mutex> lk(mu_);
      m_.guardChecks++;
      bool anyHeld = false;
      for (int k = 0; k < g.enc->surfaceCount(); k++) {
        const Surface& s = g.enc->surface(k);
        if (s.state == Surface::State::Queued) {
          anyHeld = true;
          if (!g.enc->surfaceWritable(k)) m_.guardRejections++;
        }
      }
      if (!anyHeld) m_.guardRejections++;  // no held surface: nothing to test
    }

    bool corrupt = (i == injectedSeq);
    FramePlan plan = renderFrame((uint64_t)i, g.width, g.height, corrupt);
    int64_t submitUs = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      Surface& s = g.enc->surface(idx);
      if (s.state != Surface::State::Free) {
        finishRun("failed",
                  "内部所有权冲突：填充前表面 " + std::to_string(idx) +
                      " 不是 FREE 状态");
        return;
      }
      s.state = Surface::State::Filling;
      convertRgbToSurface(plan, g.layout, s.refs, s.planeBytes);
      s.fillCount++;
      s.heldSeq = (uint64_t)i;
      s.state = Surface::State::Queued;
      Thumb src = downscaleRgb(plan.rgb.data(), plan.width, plan.height, 360);
      Artifacts& a = artifacts_[(uint64_t)i];
      a.src = std::move(src);
      artifactOrder_.push_back((uint64_t)i);
      FrameView* fv = frameFor((uint64_t)i);
      if (fv) {
        fv->status = "submitted";
        fv->surfaceIndex = idx;
        fv->gen = g.id;
        fv->hasSrc = true;
        fv->corrupted = corrupt;
        if (corrupt) fv->note = "已向待编码画面注入故障";
      }
      m_.submitted++;
      snap_.latestSeq = (uint64_t)i;
      refreshSurfacesLocked();
    }

    submitUs = nowUs();
    {
      std::lock_guard<std::mutex> lk(mu_);
      submitUs_[(uint64_t)i] = submitUs;
    }
    std::string serr;
    if (!g.enc->submit(idx, (uint64_t)i, (int64_t)i * 1000 / cfg_.fps, serr)) {
      std::lock_guard<std::mutex> lk(mu_);
      g.enc->surface(idx).state = Surface::State::Free;
      finishRun("failed", "提交帧 " + std::to_string(i) + " 失败：" + serr);
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      m_.accepted++;
      updatePeaks();
    }

    // Arm the earlyreuse fault: overwrite this surface while the backend is
    // still scanning it for this frame.
    if (i == victimSeq) {
      std::lock_guard<std::mutex> lk(mu_);
      overwritePending_ = true;
      overwriteAtUs_ =
          submitUs + (int64_t)(cfg_.readDelayMs + cfg_.scanMs / 2) * 1000;
      overwriteSurface_ = idx;
      overwritePlanSeq_ = (uint64_t)(i + 1);
      overwriteGen_ = g.id;
      logLine("故障注入已布防：帧 " + std::to_string(i) +
              " 的表面将在读取窗口内被覆写为帧 " + std::to_string(i + 1) +
              " 的内容");
    }

    // Pull anything already available so receipts are not artificially held.
    pumpAndGate();
    if (!fatal_.empty()) break;
    nextDeadlineUs += framePeriodUs;
    if (!paced) nextDeadlineUs = nowUs();
    if (nowUs() > nextDeadlineUs + 4 * framePeriodUs) nextDeadlineUs = nowUs();
    if (stopFlag_.load()) break;
  }

  // End-of-stream drain on every generation.
  {
    Generation& g = cur();
    std::string eerr;
    bool eosOk = g.enc->requestEos(eerr);
    {
      std::lock_guard<std::mutex> lk(mu_);
      g.eosRequested = true;
      snap_.eosRequested = true;
      snap_.status = "draining";
      snap_.phase = "draining";
      logLine(eosOk ? "已请求结束排空，正在取回剩余码流"
                    : "结束命令被拒绝（" + eerr + "），尽力排空");
    }
  }
  int64_t drainStartUs = nowUs();
  int64_t lastProgressUs = drainStartUs;
  for (;;) {
    if (stopFlag_.load()) break;
    if (!fatal_.empty()) break;
    maybeInjectOverwrite(nowUs());
    bool applied = pumpAndGate();
    if (!fatal_.empty()) break;
    if (applied) lastProgressUs = nowUs();
    bool allDone = true;
    size_t pendingCount = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      pendingCount = pending_.size();
      for (auto& gp : gens_) {
        if (gp->closed) continue;
        if (!gp->eosRequested || !gp->enc->eosComplete()) allDone = false;
        for (int i = 0; i < gp->enc->surfaceCount(); i++) {
          Surface::State st = gp->enc->surface(i).state;
          if (st == Surface::State::Queued || st == Surface::State::Filling)
            allDone = false;
        }
      }
      // Retire generations that have fully drained.
      for (size_t gi = 0; gi + 1 < gens_.size(); gi++) {
        Generation& old = *gens_[gi];
        if (old.closed || !old.eosRequested || !old.enc->eosComplete())
          continue;
        bool anyHeld = false;
        for (int i = 0; i < old.enc->surfaceCount(); i++) {
          Surface::State st = old.enc->surface(i).state;
          if (st == Surface::State::Queued || st == Surface::State::Filling)
            anyHeld = true;
        }
        bool pendingForGen = false;
        for (auto& pe : pending_)
          if (pe.gen == old.id) pendingForGen = true;
        if (!anyHeld && !pendingForGen) {
          logLine("代次 " + std::to_string(old.id) +
                  " 的所有表面归还均已实际确认，旧分辨率资源销毁");
          closeGeneration(old);
        }
      }
    }
    if (allDone && pendingCount == 0) break;
    // In step mode the user may hold events back indefinitely; only the
    // free-running path gets a no-progress watchdog.
    if (!stepMode_ && cfg_.script.empty() &&
        nowUs() - lastProgressUs > 5000000) {
      std::lock_guard<std::mutex> lk(mu_);
      logLine("排空 5 秒无新输出，报告为未完成排空");
      break;
    }
    sleepUs(2000);
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    bool eosAll = true;
    for (auto& gp : gens_)
      if (!gp->closed && !gp->enc->eosComplete()) eosAll = false;
    snap_.eosComplete = eosAll;
    snap_.framesDrained = m_.bitstreams;
    m_.framesDrained = m_.bitstreams;
    logLine(eosAll ? "排空完成：所有待取码流已取回，表面全部归还"
                   : "排空未完成：编码器未确认结束");
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    int leaked = 0;
    for (auto& gp : gens_) {
      if (gp->closed) continue;
      for (int i = 0; i < gp->enc->surfaceCount(); i++) {
        Surface::State st = gp->enc->surface(i).state;
        if (st != Surface::State::Free && st != Surface::State::Retired)
          leaked++;
      }
    }
    refreshSurfacesLocked();
    finalSurfaces_ = snap_.surfaces;
    snap_.leakedSurfaces = leaked;
    if (leaked)
      logLine("警告：" + std::to_string(leaked) + " 个表面始终未被编码器归还");
    pruneArtifacts();
    int64_t endedUs = nowUs();
    finishRun(fatal_.empty() ? "completed" : "failed", fatal_, endedUs);
    for (auto& gp : gens_) closeGeneration(*gp);
    snap_.encoderClosed = true;
    snap_.decoderOpen = false;
  }
}

// ---------------------------------------------------------------------------
// Views and accounting
// ---------------------------------------------------------------------------

void Session::updatePeaks() {
  // mu_ held by caller.
  int inFlight = 0, active = 0;
  size_t mapped = 0;
  for (auto& gp : gens_) {
    if (gp->closed) continue;
    for (int i = 0; i < gp->enc->surfaceCount(); i++) {
      Surface& s = gp->enc->surface(i);
      if (s.state == Surface::State::Retired) continue;
      active++;
      mapped += s.totalBytes;
      if (s.state == Surface::State::Queued) inFlight++;
    }
  }
  maxInFlightObserved_ = std::max(maxInFlightObserved_, inFlight);
  m_.maxInFlight = maxInFlightObserved_;
  m_.peakActiveSurfaces = std::max(m_.peakActiveSurfaces, active);
  snap_.mappedBytes = mapped;
  m_.surfaceBytes = mapped;
  if (mapped > m_.peakMappedBytes) m_.peakMappedBytes = mapped;
  size_t rss = processRssKb();
  if ((int)rss > m_.peakRssKb) m_.peakRssKb = (int)rss;
}

void Session::refreshSurfacesLocked() {
  snap_.surfaces.clear();
  snap_.generations.clear();
  for (auto& gp : gens_) {
    Generation& g = *gp;
    GenerationView gv;
    gv.id = g.id;
    gv.width = g.width;
    gv.height = g.height;
    if (g.closed) {
      // Keep reclaimed generations visible: their surfaces are gone, which is
      // exactly the evidence that the old pool was fully released.
      gv.state = "done";
      snap_.generations.push_back(gv);
      continue;
    }
    gv.state = g.eosRequested
                   ? (g.enc->eosComplete() ? "done" : "draining")
                   : "active";
    int freeN = 0;
    size_t bytes = 0;
    for (int i = 0; i < g.enc->surfaceCount(); i++) {
      Surface& s = g.enc->surface(i);
      SurfaceView v;
      v.index = s.index;
      v.gen = g.id;
      v.state = s.stateName();
      v.seq = s.heldSeq;
      v.reuse = s.reuseCount;
      v.fills = (int)s.fillCount;
      v.bytes = s.totalBytes;
      v.deviceMemory = s.deviceMemory;
      v.fmt = pixFmtName(s.fmt);
      v.visibleW = s.visibleW;
      v.visibleH = s.visibleH;
      v.codedW = s.codedW;
      v.codedH = s.codedH;
      v.stride0 = s.refs.stride[0];
      v.stride1 = s.refs.stride[1];
      v.stride2 = s.refs.stride[2];
      v.releaseCond = g.caps.releaseSemantics;
      snap_.surfaces.push_back(v);
      if (s.state != Surface::State::Retired) {
        gv.surfaces++;
        bytes += s.totalBytes;
        if (s.state == Surface::State::Free) freeN++;
      }
    }
    gv.freeSurfaces = freeN;
    gv.bytes = bytes;
    snap_.generations.push_back(gv);
  }
  snap_.version++;
}

void Session::handleBitstream(EncoderEvent& ev, Generation& g, int64_t now) {
  m_.bitstreams++;
  FrameView* fv = frameFor(ev.seq);
  if (!fv) {
    m_.orphanAu++;
    logLine("收到未知帧号的码流：" + std::to_string(ev.seq));
    return;
  }
  if (fv->bitstreamReady) {
    m_.duplicateSeq++;
    logLine("帧 " + std::to_string(ev.seq) + " 出现重复码流");
  }
  if (ev.surfaceIndex >= 0 && ev.seq != (uint64_t)fv->order)
    m_.outOfOrder++;
  fv->bitstreamReady = true;
  fv->auBytes = (int)ev.data.size();
  fv->keyframe = ev.keyframe;
  fv->auCrc = crc32of(ev.data);
  auto sit = submitUs_.find(ev.seq);
  if (sit != submitUs_.end() && fv->submitToAuMs < 0) {
    fv->submitToAuMs = (double)(now - sit->second) / 1000.0;
    latAu_.push_back(fv->submitToAuMs);
  }
  Artifacts& a = artifacts_[ev.seq];
  a.au = ev.data;
  a.keyframe = ev.keyframe;

  if (!g.decoderOpen || !cfg_.verify) {
    fv->status = "unverified";
    return;
  }
  YuvView view;
  std::string derr;
  bool got = g.dec.decode(ev.data, view, derr);
  fv->decodeClean = derr.empty();
  if (!derr.empty()) {
    m_.decodeErrors++;
    fv->note = derr;
  }
  if (!got) {
    m_.lost++;
    fv->status = "fail";
    if (fv->note.empty()) fv->note = "解码器未产出画面";
    return;
  }
  fv->decodedW = view.width;
  fv->decodedH = view.height;

  MarkerReading mr = readMarker(view);
  uint16_t expected = (uint16_t)(ev.seq & ((1u << kMarkerBits) - 1));
  fv->markerExpected = expected;
  fv->markerDecoded = mr.bits;
  fv->markerOk = (mr.blocks == kMarkerBits && mr.bits == expected);
  if (!fv->markerOk) m_.markerMismatches++;

  CornerReading cr = readCornerTags(view);
  fv->cornerExpected = expected;
  for (int c = 0; c < 4; c++) fv->cornerTags[c] = cr.tag[c];
  fv->cornerValid = cr.validCount;
  fv->cornersMixed = cr.mixed;
  bool cornerDataOk = cr.validCount >= 3;
  bool cornersAllExpected = cornerDataOk && !cr.mixed;
  if (cornersAllExpected)
    for (int c = 0; c < 4; c++)
      if (cr.valid[c] && cr.tag[c] != expected) cornersAllExpected = false;
  fv->cornersOk = cornersAllExpected;
  if (cr.mixed) {
    m_.mixedFrames++;
  } else if (cornerDataOk && !fv->cornersOk) {
    m_.cornerMismatches++;
  }

  uint8_t levels[3] = {255, 255, 255};
  readPatchLevels(view, levels);
  static const uint8_t want[3] = {2, 1, 3};
  fv->patchesOk =
      levels[0] == want[0] && levels[1] == want[1] && levels[2] == want[2];
  if (!fv->patchesOk) m_.patchMismatches++;

  std::vector<uint8_t> rgb = yuvToRgb(view);
  auto sampleMean = [&](int x0, int y0, int ch) -> int {
    long acc = 0;
    int n = 0;
    for (int y = y0; y < y0 + 12 && y < view.height; y++)
      for (int x = x0; x < x0 + 12 && x < view.width; x++) {
        acc += rgb[((size_t)y * view.width + x) * 3 + ch];
        n++;
      }
    return n ? (int)(acc / n) : 0;
  };
  int rm = sampleMean(10, 106, 0), gm = sampleMean(38, 106, 1),
      bm = sampleMean(66, 106, 2);
  fv->colorsOk = rm > 140 && gm > 140 && bm > 140;
  if (!fv->colorsOk) {
    m_.colorMismatches++;
    fv->note += " 颜色块均值 R/G/B=" + std::to_string(rm) + "/" +
                std::to_string(gm) + "/" + std::to_string(bm);
  }

  Thumb decThumb = downscaleRgb(rgb.data(), view.width, view.height, 360);
  auto it = artifacts_.find(ev.seq);
  double mae = -1;
  if (it != artifacts_.end() && !it->second.src.rgb.empty())
    mae = meanAbsError(it->second.src, decThumb);
  fv->meanAbsError = mae;
  bool pictureOk = mae >= 0 && mae < 12.0;
  if (!pictureOk) m_.pictureMismatches++;

  bool pass = fv->markerOk && fv->patchesOk && fv->colorsOk && pictureOk &&
              fv->decodeClean && (cornerDataOk ? fv->cornersOk : true);
  if (fv->corrupted) {
    // The injected fault must be observable: the marker check has to fail,
    // while the rest of the pipeline still decodes cleanly.
    pass = fv->decodeClean && !fv->markerOk && pictureOk;
    fv->note = fv->markerOk ? "注入的故障未被核对发现"
                            : "已通过帧号标记检出注入的故障";
  }
  if (fv->corrupted && pass) {
    fv->status = "detected";
    m_.detected++;
  } else if (cr.mixed && cfg_.mode == "earlyreuse" && m_.detected == 0) {
    // The premature-reuse fault must leave an identifiable torn frame.
    fv->status = "detected";
    m_.detected++;
    fv->note = "串帧检出：四角标 " + std::to_string(cr.tag[0]) + "/" +
               std::to_string(cr.tag[1]) + "/" + std::to_string(cr.tag[2]) +
               "/" + std::to_string(cr.tag[3]) +
               " 不一致——提前复用在解码画面留下可识别串帧";
    logLine("帧 " + std::to_string(ev.seq) + "：" + fv->note);
  } else if (pass) {
    fv->status = "verified";
    m_.verified++;
  } else {
    fv->status = "fail";
    m_.failed++;
    if (cr.mixed && fv->note.empty())
      fv->note = "四角标不一致（串帧），但本次运行未注入提前复用故障";
  }
  if (ev.seq == 0 || (fv->corrupted && !fv->markerOk)) {
    logLine("解码帧 " + std::to_string(ev.seq) + "：" +
            std::to_string(view.width) + "x" + std::to_string(view.height) +
            "，帧号标记 " + std::to_string(mr.bits) + "/" +
            std::to_string(expected) + "，平均绝对误差 " +
            std::to_string(mae));
  }
  a.out = std::move(decThumb);
  fv->hasOut = !a.out.rgb.empty();
}

void Session::handleRelease(EncoderEvent& ev, Generation& g, int64_t now) {
  m_.released++;
  FrameView* fv = frameFor(ev.seq);
  if (fv && !fv->surfaceReusable) {
    fv->surfaceReusable = true;
    auto sit = submitUs_.find(ev.seq);
    if (sit != submitUs_.end() && fv->submitToReleaseMs < 0) {
      fv->submitToReleaseMs = (double)(now - sit->second) / 1000.0;
      latRel_.push_back(fv->submitToReleaseMs);
      relLagMs_.push_back(fv->submitToReleaseMs);
      if (relLagMs_.size() > 32) relLagMs_.erase(relLagMs_.begin());
    }
  }
  if (ev.surfaceIndex >= 0 && ev.surfaceIndex < g.enc->surfaceCount()) {
    Surface& s = g.enc->surface(ev.surfaceIndex);
    if (s.state == Surface::State::Queued) {
      s.state = Surface::State::Free;
      s.reuseCount++;
      if (ev.surfaceIndex < (int)g.idleSinceUs.size())
        g.idleSinceUs[ev.surfaceIndex] = now;
    } else if (s.state == Surface::State::Retired) {
      logLine("代次 " + std::to_string(g.id) + " 表面 " +
              std::to_string(ev.surfaceIndex) +
              " 的归还到达时该表面已退役，按代次记账后忽略");
    }
    // A stale release from an old generation must never touch the new
    // pool's same-numbered surface. Detect and log the interleaving.
    if (g.id != gens_.back()->id) {
      Generation& ng = *gens_.back();
      if (!ng.closed && ev.surfaceIndex < ng.enc->surfaceCount()) {
        Surface& ns = ng.enc->surface(ev.surfaceIndex);
        if (ns.state == Surface::State::Queued ||
            ns.state == Surface::State::Filling) {
          m_.staleGenReleases++;
          logLine("旧代次 " + std::to_string(g.id) + " 表面 " +
                  std::to_string(ev.surfaceIndex) +
                  " 的归还到达时，新代次 " + std::to_string(ng.id) +
                  " 的同号表面正被占用（帧 " +
                  std::to_string(ns.heldSeq == UINT64_MAX ? -1
                                                          : (long long)ns.heldSeq) +
                  "）——按代次隔离，新表面不受影响");
        }
      }
    }
  }
  if (!ev.detail.empty()) logLine("表面回执：" + ev.detail);
  maybeShrink(g);
}

void Session::pruneArtifacts() {
  // Keep access units only for the most recent frames; the thumbnails needed
  // by the UI are small and stay for every frame.
  const size_t keepAu = 64;
  if (artifactOrder_.size() <= keepAu) return;
  for (size_t i = 0; i + keepAu < artifactOrder_.size(); i++) {
    auto it = artifacts_.find(artifactOrder_[i]);
    if (it != artifacts_.end()) {
      it->second.au.clear();
      it->second.au.shrink_to_fit();
    }
  }
}

void Session::finishRun(const std::string& status, const std::string& error) {
  finishRun(status, error, nowUs());
}

void Session::finishRun(const std::string& status, const std::string& error,
                        int64_t endedUs) {
  snap_.status = status;
  snap_.error = error;
  if (!error.empty()) logLine("错误：" + error);

  for (auto& f : frames_) {
    if (f.status == "pending" || f.status == "submitted") {
      f.status = "skipped";
      m_.skipped++;
    }
  }
  m_.maxInFlight = std::max(m_.maxInFlight, maxInFlightObserved_);
  m_.p50SubmitToAuMs = percentile(latAu_, 0.5);
  m_.maxSubmitToAuMs = percentile(latAu_, 1.0);
  m_.p50SubmitToReleaseMs = percentile(latRel_, 0.5);
  m_.maxSubmitToReleaseMs = percentile(latRel_, 1.0);
  updatePeaks();
  snap_.metrics = m_;
  snap_.frames = frames_;
  if (!finalSurfaces_.empty()) snap_.surfaces = finalSurfaces_;
  snap_.endedAtUs = endedUs;
  snap_.phase = "done";

  int expected = cfg_.frames;
  bool orderOk = m_.outOfOrder == 0 && m_.orphanAu == 0 &&
                 m_.duplicateSeq == 0;
  bool allReleased = snap_.leakedSurfaces == 0;
  bool encodeCountOk = m_.bitstreams >= expected;
  int injected =
      (cfg_.mode == "corrupt" || cfg_.mode == "earlyreuse") ? 1 : 0;
  bool pass = m_.verified >= expected - injected && m_.failed == 0 &&
              m_.detected == injected && m_.lost == 0 && orderOk &&
              allReleased && encodeCountOk &&
              (snap_.verifyEnabled ? (m_.verified + m_.failed + m_.detected ==
                                      expected)
                                   : true);
  snap_.pipelineVerified = pass && status == "completed";
  if (snap_.pipelineVerified) {
    snap_.verificationNote =
        "送帧链路已证明：每个提交帧都产出访问单元，解码得到的画面在帧号、角标、参考色块、颜色与内容上都与源画面一致，且所有表面都已归还 FREE。";
  } else if (status == "completed") {
    snap_.verificationNote =
        "运行结束但未达验收：通过 " +
        std::to_string(m_.verified) + "/" + std::to_string(expected) +
        "，失败 " + std::to_string(m_.failed) + "，检出 " +
        std::to_string(m_.detected) + "，丢失 " +
        std::to_string(m_.lost) + "，乱序 " + std::to_string(m_.outOfOrder) +
        "，串帧 " + std::to_string(m_.mixedFrames) + "，未归还表面 " +
        std::to_string(snap_.leakedSurfaces);
  } else {
    snap_.verificationNote = "运行未完成：" + error;
  }
  if (!snap_.hardwareCovered) {
    snap_.verificationNote +=
        " 硬件覆盖：本机没有可用硬件编码器，本次只验证了软件对照与接口模型，不报告任何硬件吞吐数据。";
  } else {
    snap_.verificationNote +=
        " 硬件覆盖：本次使用真实 V4L2 M2M 编码节点。";
  }

  // Capacity rationale from the measured release feedback.
  double periodMs = 1000.0 / std::max(1, cfg_.fps);
  if (m_.p50SubmitToReleaseMs >= 0) {
    double lag = m_.p50SubmitToReleaseMs;
    int k = (int)std::ceil(lag / periodMs);
    std::string h = "释放滞后 p50 " + std::to_string((int)(lag + 0.5)) +
                    " ms ≈ " + std::to_string(k) + " 个帧间隔（" +
                    std::to_string((int)(periodMs + 0.5)) + " ms）";
    h += "：稳态在途约 " + std::to_string(k) +
         " 个表面，固定池容量建议 ≥ " + std::to_string(k + 1) + "。";
    if (cfg_.sched == "elastic") {
      h += " 本次弹性池：初始 " + std::to_string(cfg_.pool) + "，峰值活跃 " +
           std::to_string(m_.peakActiveSurfaces) + "，增长 " +
           std::to_string(m_.grows) + " 次、收缩 " +
           std::to_string(m_.shrinks) + " 次、选择等待 " +
           std::to_string(m_.schedWaits) + " 次。";
    } else {
      h += " 本次固定池 " + std::to_string(cfg_.pool) + "，阻塞 " +
           std::to_string(m_.backpressureStalls) + " 次 / 累计 " +
           std::to_string((int)(m_.stallMsTotal + 0.5)) + " ms。";
    }
    h += " 选定规则：预计归还超过半帧预算才增长，否则等待；空闲满 8 个帧周期则收缩。";
    if (stepMode_ || !cfg_.script.empty())
      h += "（本次含单步/脚本停顿，时延数值含人为停顿，容量结论以自由重放为准。）";
    h += snap_.timingSource == "device"
             ? "（时序来自真实设备。）"
             : "（时序来自软件模型模拟，非设备实测。）";
    snap_.capacityHint = h;
  } else {
    snap_.capacityHint = "未测得释放滞后，无法给出容量建议。";
  }
  snap_.version++;
}

SessionSnapshot Session::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  return snap_;
}

bool Session::framePng(const std::string& kind, uint64_t seq, int maxWidth,
                       std::vector<uint8_t>& png) const {
  std::lock_guard<std::mutex> lk(mu_);
  uint64_t target = seq;
  if (target == UINT64_MAX) {
    if (snap_.latestSeq == UINT64_MAX) return false;
    target = snap_.latestSeq;
  }
  auto it = artifacts_.find(target);
  if (it == artifacts_.end()) return false;
  const Thumb* t = kind == "src" ? &it->second.src : &it->second.out;
  if (t->rgb.empty()) return false;
  Thumb scaled = *t;
  if (maxWidth > 0 && t->width > maxWidth)
    scaled = downscaleRgb(t->rgb.data(), t->width, t->height, maxWidth);
  png = encodePngRgb(scaled.rgb.data(), scaled.width, scaled.height);
  return !png.empty();
}

bool Session::accessUnit(uint64_t seq, std::vector<uint8_t>& data,
                         bool& keyframe) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = artifacts_.find(seq);
  if (it == artifacts_.end() || it->second.au.empty()) return false;
  data = it->second.au;
  keyframe = it->second.keyframe;
  return true;
}
