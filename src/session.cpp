#include "session.h"

#include <cstring>
#include <unordered_map>

#include "png.h"

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
      c.mode != "guard" && c.mode != "corrupt") {
    err = "unknown send mode";
    return false;
  }
  if (c.receiptLagMs < 0 || c.receiptLagMs > 2000) {
    err = "receiptLag must be 0..2000 ms";
    return false;
  }
  if (c.backend != "openh264" && c.backend != "v4l2") {
    err = "unknown backend";
    return false;
  }
  if (c.mode == "late" && c.receiptLagMs == 0) c.receiptLagMs = 120;

  SessionSnapshot fresh;
  fresh.status = "starting";
  fresh.cfg = c;
  fresh.phase = "initialising";
  fresh.startedAtUs = nowUs();

  std::lock_guard<std::mutex> lk(mu_);
  cfg_ = c;
  snap_ = std::move(fresh);
  frames_.clear();
  finalSurfaces_.clear();
  artifacts_.clear();
  artifactOrder_.clear();
  submitUs_.clear();
  m_ = RunMetrics();
  maxInFlightObserved_ = 0;
  latAu_.clear();
  latRel_.clear();
  fatal_.clear();
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

void Session::logLine(const std::string& s) {
  snap_.log.push_back(s);
  if (snap_.log.size() > 300) snap_.log.erase(snap_.log.begin());
}

FrameView* Session::frameFor(uint64_t seq) {
  if (seq < frames_.size()) return &frames_[seq];
  return nullptr;
}

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
            "，表面池=" + std::to_string(cfg_.pool) + "，帧数=" +
            std::to_string(cfg_.frames));
  }

  EncoderSettings es;
  es.width = cfg_.width;
  es.height = cfg_.height;
  es.fps = cfg_.fps;
  es.bitrate = cfg_.bitrate;
  es.gop = cfg_.gop;
  es.pool = cfg_.pool;
  es.format = cfg_.format;
  es.device = cfg_.device;
  es.receiptLagMs = cfg_.backend == "v4l2" ? 0 : cfg_.receiptLagMs;

  if (cfg_.backend == "v4l2")
    enc_ = makeV4l2Encoder(es, err);
  else
    enc_ = makeOpenH264Encoder(es, err);
  if (!enc_) {
    std::lock_guard<std::mutex> lk(mu_);
    finishRun("failed", "后端构造失败：" + err);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    caps_ = enc_->caps();
    snap_.caps = caps_;
    snap_.sessionCreated = true;
    snap_.hardwareCovered = caps_.hardware;
    logLine("编码会话已创建：" + caps_.displayName);
    logLine("会话创建并不等于送帧链路正确；下面分别检查提交、异步码流回执、解码核对与表面归还。");
  }

  if (!enc_->createPool(cfg_.pool, err)) {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.caps = enc_->caps();
    finishRun("failed", "无法分配所需表面：" + err);
    closeEncoder();
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    caps_ = enc_->caps();
    snap_.caps = caps_;
    format_ = enc_->format();
    snap_.format = format_;
    layout_.fmt = format_.fmt;
    layout_.visibleW = format_.visibleW;
    layout_.visibleH = format_.visibleH;
    layout_.codedW = format_.codedW;
    layout_.codedH = format_.codedH;
    layout_.yStride = format_.stride0;
    layout_.uvStride = format_.stride1;
    layout_.uvRows = format_.codedH / 2;
    size_t total = 0;
    for (int i = 0; i < enc_->surfaceCount(); i++) {
      Surface& s = enc_->surface(i);
      total += s.totalBytes;
      logLine("表面 " + std::to_string(i) + "：" + pixFmtName(s.fmt) +
              " 可视 " + std::to_string(s.visibleW) + "x" +
              std::to_string(s.visibleH) + " 编码区 " +
              std::to_string(s.codedW) + "x" + std::to_string(s.codedH) +
              " 步幅 " + std::to_string(s.refs.stride[0]) + "/" +
              std::to_string(s.refs.stride[1]) + "/" +
              std::to_string(s.refs.stride[2]) +
              (s.deviceMemory ? " 设备映射" : " 主机内存") + "，" +
              std::to_string(s.totalBytes) + " 字节");
    }
    m_.surfaceBytes = total;
    snap_.mappedBytes = total;
    snap_.status = "running";
    snap_.phase = "feeding frames";
    logLine("输出格式 " + format_.pixTag + "，编码区 " +
            std::to_string(format_.codedW) + "x" +
            std::to_string(format_.codedH) + "，步幅 " +
            std::to_string(format_.stride0) + "/" +
            std::to_string(format_.stride1) +
            (format_.grantedDiffers
                 ? "（驱动给出的编码区与请求不同，转换按实际步幅写入）"
                 : ""));
  }

  if (cfg_.verify) {
    snap_.verifyEnabled = true;
    if (!dec_.open(err)) {
      std::lock_guard<std::mutex> lk(mu_);
      logLine("解码器打开失败（" + err + "），本次运行无法做画面核对");
    } else {
      std::lock_guard<std::mutex> lk(mu_);
      snap_.decoderOpen = true;
    }
  }

  const int64_t framePeriodUs = 1000000 / std::max(1, cfg_.fps);
  const bool paced = cfg_.mode == "paced" || cfg_.mode == "late" ||
                     cfg_.mode == "guard" || cfg_.mode == "corrupt";
  int64_t nextDeadlineUs = runStartUs_;
  const int injectedSeq =
      cfg_.mode == "corrupt" ? std::min(2, cfg_.frames - 1) : -1;
  int submitted = 0;

  for (int i = 0; i < cfg_.frames; i++) {
    if (stopFlag_.load() && i > 0) break;
    if (paced) {
      while (true) {
        int64_t wait = nextDeadlineUs - nowUs();
        if (wait <= 0) break;
        std::vector<EncoderEvent> evs;
        int n = enc_->poll(evs, (int)std::min<int64_t>(wait / 1000 + 1, 30),
                           err);
        if (n < 0) {
          std::lock_guard<std::mutex> lk(mu_);
          finishRun("failed", err);
          closeEncoder();
          return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        processEvents(evs, nowUs());
        if (stopFlag_.load()) break;
      }
      if (stopFlag_.load() && i > 0) break;
    }

    // Pick a FREE surface, or wait for one to come back from the codec.
    int idx = -1;
    int64_t stallStartUs = nowUs();
    while (true) {
      bool found = false;
      for (int k = 0; k < enc_->surfaceCount(); k++) {
        if (enc_->surface(k).state == Surface::State::Free) {
          idx = k;
          found = true;
          break;
        }
      }
      if (found) break;
      if (stopFlag_.load()) break;
      std::vector<EncoderEvent> evs;
      int n = enc_->poll(evs, 5, err);
      if (n < 0) {
        std::lock_guard<std::mutex> lk(mu_);
        finishRun("failed", err);
        closeEncoder();
        return;
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        processEvents(evs, nowUs());
      }
      if (nowUs() - stallStartUs > 15000000) {
        std::lock_guard<std::mutex> lk(mu_);
        finishRun("failed",
                  "15 秒内没有任何表面归还（池已耗尽且回执停止）");
        closeEncoder();
        return;
      }
    }
    if (idx < 0) break;
    int64_t stallUs = nowUs() - stallStartUs;
    if (stallUs > 1500) {
      std::lock_guard<std::mutex> lk(mu_);
      m_.backpressureStalls++;
      m_.stallMsTotal += (double)stallUs / 1000.0;
      if (m_.backpressureStalls <= 6)
        logLine("背压：帧 " + std::to_string(i) + " 等待 " +
                std::to_string(stallUs / 1000) + " ms 才拿到空闲表面；" +
                std::to_string(enc_->surfaceCount()) +
                " 个表面都仍被编码器持有");
    }

    // In guard mode, verify the ownership semantics the hard way: a surface
    // still held by the codec must be reported non-writable.
    if (cfg_.mode == "guard") {
      std::lock_guard<std::mutex> lk(mu_);
      m_.guardChecks++;
      bool anyHeld = false;
      for (int k = 0; k < enc_->surfaceCount(); k++) {
        const Surface& s = enc_->surface(k);
        if (s.state == Surface::State::Queued) {
          anyHeld = true;
          if (!enc_->surfaceWritable(k)) m_.guardRejections++;
        }
      }
      if (!anyHeld) m_.guardRejections++;  // no held surface: nothing to test
    }

    bool corrupt = (i == injectedSeq);
    FramePlan plan = renderFrame((uint64_t)i, cfg_.width, cfg_.height, corrupt);
    int64_t submitUs = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      Surface& s = enc_->surface(idx);
      if (s.state != Surface::State::Free) {
        finishRun("failed",
                  "内部所有权冲突：填充前表面 " + std::to_string(idx) +
                      " 不是 FREE 状态");
        closeEncoder();
        return;
      }
      s.state = Surface::State::Filling;
      convertRgbToSurface(plan, layout_, s.refs, s.planeBytes);
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
    if (!enc_->submit(idx, (uint64_t)i, (int64_t)i * 1000 / cfg_.fps, serr)) {
      std::lock_guard<std::mutex> lk(mu_);
      enc_->surface(idx).state = Surface::State::Free;
      finishRun("failed", "提交帧 " + std::to_string(i) + " 失败：" + serr);
      closeEncoder();
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      m_.accepted++;
      int inFlight = 0;
      for (int k = 0; k < enc_->surfaceCount(); k++)
        if (enc_->surface(k).state == Surface::State::Queued) inFlight++;
      maxInFlightObserved_ = std::max(maxInFlightObserved_, inFlight);
      m_.maxInFlight = maxInFlightObserved_;
    }
    submitted++;

    // Pull anything already available so receipts are not artificially held.
    std::vector<EncoderEvent> evs;
    int n = enc_->poll(evs, 0, err);
    if (n < 0) {
      std::lock_guard<std::mutex> lk(mu_);
      finishRun("failed", err);
      closeEncoder();
      return;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      processEvents(evs, nowUs());
    }
    nextDeadlineUs += framePeriodUs;
    if (!paced) nextDeadlineUs = nowUs();
    if (nowUs() > nextDeadlineUs + 4 * framePeriodUs) nextDeadlineUs = nowUs();
    if (stopFlag_.load()) break;
  }

  (void)submitted;

  // End-of-stream drain.
  {
    std::string eerr;
    bool eosOk = enc_->requestEos(eerr);
    int64_t drainStartUs = nowUs();
    int64_t lastProgressUs = drainStartUs;
    {
      std::lock_guard<std::mutex> lk(mu_);
      snap_.eosRequested = true;
      snap_.status = "draining";
      snap_.phase = "draining";
      logLine(eosOk ? "已请求结束排空，正在取回剩余码流"
                    : "结束命令被拒绝（" + eerr + "），尽力排空");
    }
    for (;;) {
      if (stopFlag_.load()) break;
      std::vector<EncoderEvent> evs;
      int n = enc_->poll(evs, 10, err);
      if (n < 0) {
        std::lock_guard<std::mutex> lk(mu_);
        finishRun("failed", err);
        closeEncoder();
        return;
      }
      int64_t t = nowUs();
      {
        std::lock_guard<std::mutex> lk(mu_);
        int before = m_.bitstreams + m_.released;
        processEvents(evs, t);
        if (m_.bitstreams + m_.released != before) lastProgressUs = t;
      }
      if (enc_->eosComplete()) break;
      if (t - lastProgressUs > 5000000) {
        std::lock_guard<std::mutex> lk(mu_);
        logLine("排空 5 秒无新输出，报告为未完成排空");
        break;
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    snap_.eosComplete = enc_->eosComplete();
    snap_.framesDrained = m_.bitstreams;
    m_.framesDrained = m_.bitstreams;
    logLine(snap_.eosComplete
                ? "排空完成：所有待取码流已取回，表面全部归还"
                : "排空未完成：编码器未确认结束");
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    int leaked = 0;
    refreshSurfacesLocked();
    finalSurfaces_ = snap_.surfaces;
    for (int i = 0; i < enc_->surfaceCount(); i++)
      if (enc_->surface(i).state != Surface::State::Free) leaked++;
    snap_.leakedSurfaces = leaked;
    if (leaked)
      logLine("警告：" + std::to_string(leaked) + " 个表面始终未被编码器归还");
    pruneArtifacts();
    int64_t endedUs = nowUs();
    closeEncoder();
    finishRun("completed", "", endedUs);
  }
}

void Session::refreshSurfacesLocked() {
  if (!enc_) return;
  snap_.surfaces.clear();
  for (int i = 0; i < enc_->surfaceCount(); i++) {
    Surface& s = enc_->surface(i);
    SurfaceView v;
    v.index = s.index;
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
    v.releaseCond = caps_.releaseSemantics;
    snap_.surfaces.push_back(v);
  }
  snap_.version++;
}

void Session::processEvents(std::vector<EncoderEvent>& evs, int64_t now) {
  for (auto& ev : evs) {
    if (ev.type == EncoderEvent::Type::BitstreamReady) {
      handleBitstream(ev, now);
    } else if (ev.type == EncoderEvent::Type::SurfaceReleased) {
      handleRelease(ev, now);
    } else if (ev.type == EncoderEvent::Type::Fatal) {
      fatal_ = ev.detail;
      logLine("后端致命错误：" + ev.detail);
    }
  }
  size_t rss = processRssKb();
  if ((int)rss > m_.peakRssKb) m_.peakRssKb = (int)rss;
  refreshSurfacesLocked();
}

void Session::handleBitstream(EncoderEvent& ev, int64_t now) {
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

  if (!snap_.decoderOpen || !cfg_.verify) {
    fv->status = "unverified";
    return;
  }
  YuvView view;
  std::string derr;
  bool got = dec_.decode(ev.data, view, derr);
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
              fv->decodeClean;
  if (fv->corrupted) {
    // The injected fault must be observable: the marker check has to fail,
    // while the rest of the pipeline still decodes cleanly.
    pass = fv->decodeClean && !fv->markerOk && pictureOk;
    fv->note = fv->markerOk ? "注入的故障未被核对发现"
                            : "已通过帧号标记检出注入的故障";
  }
  if (pass && fv->corrupted) {
    fv->status = "detected";
    m_.detected++;
  } else if (pass) {
    fv->status = "verified";
    m_.verified++;
  } else {
    fv->status = "fail";
    m_.failed++;
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

void Session::handleRelease(EncoderEvent& ev, int64_t now) {
  m_.released++;
  FrameView* fv = frameFor(ev.seq);
  if (fv && !fv->surfaceReusable) {
    fv->surfaceReusable = true;
    auto sit = submitUs_.find(ev.seq);
    if (sit != submitUs_.end() && fv->submitToReleaseMs < 0) {
      fv->submitToReleaseMs = (double)(now - sit->second) / 1000.0;
      latRel_.push_back(fv->submitToReleaseMs);
    }
  }
  if (!ev.detail.empty()) logLine("表面回执：" + ev.detail);
}

void Session::closeEncoder() {
  if (enc_) enc_->shutdown();
  enc_.reset();
  dec_.close();
  snap_.decoderOpen = false;
  snap_.encoderClosed = true;
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
  size_t rss = processRssKb();
  if ((int)rss > m_.peakRssKb) m_.peakRssKb = (int)rss;
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
  int injected = cfg_.mode == "corrupt" ? 1 : 0;
  bool pass = m_.verified >= expected - injected && m_.failed == 0 &&
              m_.detected == injected && m_.lost == 0 && orderOk &&
              allReleased && encodeCountOk &&
              (snap_.verifyEnabled ? (m_.verified + m_.failed + m_.detected ==
                                      expected)
                                   : true);
  snap_.pipelineVerified = pass && status == "completed";
  if (snap_.pipelineVerified) {
    snap_.verificationNote =
        "送帧链路已证明：每个提交帧都产出访问单元，解码得到的画面在帧号、参考色块、颜色与内容上都与源画面一致，且所有表面都已归还 FREE。";
  } else if (status == "completed") {
    snap_.verificationNote =
        "运行结束但未达验收：通过 " +
        std::to_string(m_.verified) + "/" + std::to_string(expected) +
        "，失败 " + std::to_string(m_.failed) + "，检出 " +
        std::to_string(m_.detected) + "，丢失 " +
        std::to_string(m_.lost) + "，乱序 " + std::to_string(m_.outOfOrder) +
        "，未归还表面 " + std::to_string(snap_.leakedSurfaces);
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
