#include "encoder.h"

#include <wels/codec_api.h>

#include <deque>
#include <thread>

namespace {

// One submitted frame travelling through the modelled device: the input
// surface is scanned in (DMA) after readDelayMs, the scan takes scanMs,
// the packet becomes readable when the scan completes, and the surface is
// released at receiptLagMs. All three delays are harness timers that make
// the asynchronous ownership semantics of a hardware M2M encoder
// exercisable without a device; they are never reported as device latency.
struct Job {
  int surfaceIdx = -1;
  uint64_t seq = 0;
  int64_t scanStartUs = 0;
  int64_t scanDoneUs = 0;
  int64_t releaseUs = 0;
  int stripsTotal = 0;
  int stripsDone = 0;
  bool scanStarted = false;
  bool encoded = false;
  bool released = false;
  std::vector<uint8_t> staging;
};

// Software reference encoder.
//
// Receipt semantics: with readDelayMs==0 and scanMs==0 the bitstream is
// complete when submit() returns. Otherwise submit() only queues the job:
// the surface is copied strip by strip into a private staging buffer on the
// modelled DMA schedule and encoded when the scan completes. In both cases
// the input surface stays owned by the backend until the deferred-release
// timer fires and a SurfaceReleased event is emitted; the session flips the
// surface state when it applies that event. Refilling a surface while a job
// is still scanning it genuinely tears the encoded picture, exactly like
// overwriting a buffer a hardware encoder has not DQBUF'd yet.
class OpenH264Encoder : public IEncoder {
 public:
  OpenH264Encoder(const EncoderSettings& s) : s_(s) {
    caps_.kind = EncoderKind::OpenH264Software;
    caps_.backendId = "openh264";
    OpenH264Version v = WelsGetCodecVersion();
    caps_.displayName = "OpenH264 software (interface model) v" +
                        std::to_string(v.uMajor) + "." +
                        std::to_string(v.uMinor) + "." +
                        std::to_string(v.uRevision);
    caps_.hardware = false;
    caps_.deferredRelease = true;
    caps_.releaseSemantics =
        "SurfaceReleased 携带确切帧号；即使码流已可读，也必须等到该事件"
        "之后才能重填表面。";
    caps_.receiptSemantics =
        "码流在输入扫描完成后产生（readDelay/scan 为实验台定时器）；"
        "表面在 receiptLag 定时器到期前保持不可写。这些延迟是接口模型，"
        "不是设备时延。";
    caps_.maxPool = 8;
    caps_.inputFormats = {"I420"};
    caps_.notes.push_back(
        "软件编码器既作为可执行对照，也作为异步硬件回执的接口模型。");
    caps_.notes.push_back(
        "支持弹性表面池：增长即真实分配，收缩即真实释放（主机内存）。");
    caps_.limitations.push_back(
        "软件编码：吞吐与时延都是 CPU 数值与实验台定时器，不是设备测量。");
  }

  ~OpenH264Encoder() override { shutdown(); }

  EncoderCaps& caps() override { return caps_; }
  const SurfaceFormat& format() const override { return fmt_; }

  bool createPool(int count, std::string& err) override {
    if (count < 2 || count > 8) {
      err = "pool size must be 2..8";
      return false;
    }
    if (count > caps_.maxPool) {
      err = "pool exceeds backend maximum";
      return false;
    }
    if (enc_) {
      err = "pool already created";
      return false;
    }
    enc_ = nullptr;
    if (WelsCreateSVCEncoder(&enc_) != 0 || !enc_) {
      err = "WelsCreateSVCEncoder failed";
      return false;
    }
    int quiet = WELS_LOG_QUIET;
    enc_->SetOption(ENCODER_OPTION_TRACE_LEVEL, &quiet);
    SEncParamExt p;
    memset(&p, 0, sizeof p);
    enc_->GetDefaultParams(&p);
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = s_.width;
    p.iPicHeight = s_.height;
    p.iTargetBitrate = s_.bitrate;
    p.iRCMode = RC_BITRATE_MODE;
    p.fMaxFrameRate = (float)s_.fps;
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.sSpatialLayers[0].iVideoWidth = s_.width;
    p.sSpatialLayers[0].iVideoHeight = s_.height;
    p.sSpatialLayers[0].fFrameRate = (float)s_.fps;
    p.sSpatialLayers[0].iSpatialBitrate = s_.bitrate;
    p.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    p.iComplexityMode = LOW_COMPLEXITY;
    p.uiIntraPeriod = (unsigned int)s_.gop;
    p.iNumRefFrame = 1;
    p.bEnableFrameSkip = false;
    p.iMultipleThreadIdc = 1;
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.iEntropyCodingModeFlag = 0;
    p.bEnableAdaptiveQuant = false;
    p.bEnableSceneChangeDetect = false;
    p.bEnableBackgroundDetection = false;
    p.bEnableDenoise = false;
    if (enc_->InitializeExt(&p) != 0) {
      err = "OpenH264 InitializeExt failed";
      shutdown();
      return false;
    }
    int vid = videoFormatI420;
    enc_->SetOption(ENCODER_OPTION_DATAFORMAT, &vid);

    fmt_.fmt = PixFmt::I420;
    fmt_.pixTag = "I420";
    fmt_.requestedW = s_.width;
    fmt_.requestedH = s_.height;
    fmt_.visibleW = s_.width;
    fmt_.visibleH = s_.height;
    // OpenH264 internally pads to 16; we mirror that as the coded extent and
    // add a deliberately larger, aligned stride so layouts are non-trivial.
    fmt_.codedW = ((s_.width + 15) / 16) * 16;
    fmt_.codedH = ((s_.height + 15) / 16) * 16;
    fmt_.stride0 = ((fmt_.codedW + 63) / 64) * 64;
    fmt_.stride1 = ((fmt_.codedW / 2 + 63) / 64) * 64;
    fmt_.stride2 = fmt_.stride1;
    fmt_.grantedDiffers = fmt_.codedW != s_.width || fmt_.codedH != s_.height ||
                          fmt_.stride0 != fmt_.codedW;

    surfaces_.resize(count);
    for (int i = 0; i < count; i++) initSurface(surfaces_[i], i);
    return true;
  }

  int surfaceCount() const override { return (int)surfaces_.size(); }
  Surface& surface(int i) override { return surfaces_[i]; }

  bool growPool(std::string& err) override {
    int ceiling = s_.poolMax > 0 ? s_.poolMax : caps_.maxPool;
    if (ceiling > caps_.maxPool) ceiling = caps_.maxPool;
    if ((int)surfaces_.size() >= ceiling) {
      err = "pool already at ceiling";
      return false;
    }
    surfaces_.emplace_back();
    initSurface(surfaces_.back(), (int)surfaces_.size() - 1);
    return true;
  }

  bool shrinkPool(int surfaceIdx, std::string& err) override {
    if (surfaceIdx < 0 || surfaceIdx >= (int)surfaces_.size()) {
      err = "bad surface index";
      return false;
    }
    Surface& sf = surfaces_[surfaceIdx];
    if (sf.state != Surface::State::Free) {
      err = "surface not FREE";
      return false;
    }
    if (jobFor(surfaceIdx) != nullptr) {
      err = "surface still owned by a job";
      return false;
    }
    sf.memory.clear();
    sf.memory.shrink_to_fit();
    sf.refs.p[0] = sf.refs.p[1] = sf.refs.p[2] = nullptr;
    sf.state = Surface::State::Retired;
    return true;
  }

  bool submit(int surfaceIdx, uint64_t seq, int64_t ptsUs,
              std::string& err) override {
    if (!enc_) {
      err = "encoder not initialised";
      return false;
    }
    if (surfaceIdx < 0 || surfaceIdx >= (int)surfaces_.size()) {
      err = "bad surface index";
      return false;
    }
    Surface& sf = surfaces_[surfaceIdx];
    if (sf.state != Surface::State::Queued) {
      err = "surface not in QUEUED state";
      return false;
    }
    const bool deferred = s_.readDelayMs > 0 || s_.scanMs > 0;
    Job job;
    job.surfaceIdx = surfaceIdx;
    job.seq = seq;
    int64_t now = nowUs();
    if (!deferred) {
      // Synchronous read: encode from the surface right now, packet ready
      // immediately, surface released by the receipt timer alone.
      if (!encodeFrom(sf.refs.p, surfaceIdx, seq, ptsUs, err)) return false;
      job.encoded = true;
      job.scanStarted = true;
      job.stripsTotal = job.stripsDone = 1;
      job.scanStartUs = job.scanDoneUs = now;
      job.releaseUs = now + (int64_t)s_.receiptLagMs * 1000;
      jobs_.push_back(std::move(job));
      return true;
    }
    job.scanStartUs = now + (int64_t)s_.readDelayMs * 1000;
    job.scanDoneUs = job.scanStartUs + (int64_t)s_.scanMs * 1000;
    job.releaseUs = now + (int64_t)s_.receiptLagMs * 1000;
    if (job.releaseUs < job.scanDoneUs) job.releaseUs = job.scanDoneUs;
    job.stripsTotal = s_.scanMs > 0 ? 8 : 1;
    jobs_.push_back(std::move(job));
    return true;
  }

  int poll(std::vector<EncoderEvent>& out, int timeoutMs, std::string&) override {
    int64_t t = nowUs();
    // Advance every job's DMA scan / encode / release schedule.
    for (auto& j : jobs_) {
      if (!j.scanStarted && t >= j.scanStartUs) {
        Surface& sf = surfaces_[j.surfaceIdx];
        j.staging.assign(sf.totalBytes, 0);
        j.scanStarted = true;
        j.stripsDone = 0;
      }
      if (j.scanStarted && !j.encoded) {
        int64_t span = j.scanDoneUs - j.scanStartUs;
        int due = j.stripsTotal;
        if (span > 0) {
          int64_t elapsed = t - j.scanStartUs;
          due = (int)((elapsed * j.stripsTotal) / span);
          if (due > j.stripsTotal) due = j.stripsTotal;
        }
        if (due > j.stripsDone) {
          copyStrips(j, j.stripsDone, due);
          j.stripsDone = due;
        }
        if (j.stripsDone >= j.stripsTotal) {
          Surface& sf = surfaces_[j.surfaceIdx];
          PlaneRefs refs;
          refs.p[0] = j.staging.data();
          refs.p[1] = j.staging.data() + sf.planeBytes[0];
          refs.p[2] = refs.p[1] + sf.planeBytes[1];
          refs.stride[0] = sf.refs.stride[0];
          refs.stride[1] = sf.refs.stride[1];
          refs.stride[2] = sf.refs.stride[2];
          std::string encErr;
          int64_t pts = (int64_t)(j.seq * 1000 / std::max(1, s_.fps));
          if (!encodeFrom(refs.p, j.surfaceIdx, j.seq, pts, encErr)) {
            EncoderEvent fatal;
            fatal.type = EncoderEvent::Type::Fatal;
            fatal.detail = "deferred encode failed: " + encErr;
            fatal.tUs = t;
            out.push_back(std::move(fatal));
            j.encoded = true;  // do not retry
          } else {
            j.encoded = true;
          }
          j.staging.clear();
          j.staging.shrink_to_fit();
        }
      }
    }
    // Emit releases that came due.
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      Job& j = *it;
      if (j.encoded && !j.released && t >= j.releaseUs) {
        j.released = true;
        EncoderEvent ev;
        ev.type = EncoderEvent::Type::SurfaceReleased;
        ev.surfaceIndex = j.surfaceIdx;
        ev.seq = j.seq;
        ev.tUs = t;
        out.push_back(std::move(ev));
      }
      if (j.encoded && j.released)
        it = jobs_.erase(it);
      else
        ++it;
    }
    while (!ready_.empty()) {
      out.push_back(std::move(ready_.front()));
      ready_.pop_front();
    }
    eosDone_ = eos_ && jobs_.empty() && ready_.empty();
    if (out.empty() && timeoutMs > 0) {
      // Nothing due right now: avoid a busy spin in the caller's wait loop.
      // Keep the quantum small so strip schedules stay accurate.
      sleepUs(std::min<int64_t>((int64_t)timeoutMs * 1000, 2000));
    }
    return (int)out.size();
  }

  bool requestEos(std::string&) override {
    eos_ = true;
    eosDone_ = jobs_.empty() && ready_.empty();
    return true;
  }
  bool eosComplete() const override { return eosDone_; }

  bool surfaceWritable(int idx) const override {
    // Backend-owned view: a surface is writable only while no job owns it.
    return jobFor(idx) == nullptr;
  }

  std::vector<uint8_t> codecConfig() const override { return config_; }

  void shutdown() override {
    if (enc_) {
      enc_->Uninitialize();
      WelsDestroySVCEncoder(enc_);
      enc_ = nullptr;
    }
    surfaces_.clear();
    jobs_.clear();
    ready_.clear();
  }

 private:
  void initSurface(Surface& sf, int i) {
    sf.index = i;
    sf.fmt = PixFmt::I420;
    sf.visibleW = fmt_.visibleW;
    sf.visibleH = fmt_.visibleH;
    sf.codedW = fmt_.codedW;
    sf.codedH = fmt_.codedH;
    sf.planeCount = 3;
    sf.refs.stride[0] = fmt_.stride0;
    sf.refs.stride[1] = fmt_.stride1;
    sf.refs.stride[2] = fmt_.stride2;
    sf.planeBytes[0] = (size_t)fmt_.stride0 * fmt_.codedH;
    sf.planeBytes[1] = (size_t)fmt_.stride1 * (fmt_.codedH / 2);
    sf.planeBytes[2] = sf.planeBytes[1];
    sf.totalBytes = sf.planeBytes[0] + sf.planeBytes[1] + sf.planeBytes[2];
    sf.memory.assign(sf.totalBytes, 0);
    sf.refs.p[0] = sf.memory.data();
    sf.refs.p[1] = sf.memory.data() + sf.planeBytes[0];
    sf.refs.p[2] = sf.refs.p[1] + sf.planeBytes[1];
    sf.state = Surface::State::Free;
  }

  const Job* jobFor(int surfaceIdx) const {
    for (auto& j : jobs_)
      if (j.surfaceIdx == surfaceIdx && !(j.encoded && j.released)) return &j;
    return nullptr;
  }

  // Copies scan strips [from, to) of every plane from the live surface into
  // the job's staging buffer. A strip is a horizontal band; tearing between
  // strips is exactly what happens when the surface is refilled mid-scan.
  void copyStrips(Job& j, int from, int to) {
    Surface& sf = surfaces_[j.surfaceIdx];
    const int lumaRows = fmt_.codedH;
    const int chromaRows = fmt_.codedH / 2;
    for (int s = from; s < to; s++) {
      int y0 = (int)((int64_t)s * lumaRows / j.stripsTotal);
      int y1 = (int)((int64_t)(s + 1) * lumaRows / j.stripsTotal);
      for (int y = y0; y < y1; y++) {
        memcpy(j.staging.data() + (size_t)y * sf.refs.stride[0],
               sf.refs.p[0] + (size_t)y * sf.refs.stride[0],
               (size_t)sf.refs.stride[0]);
      }
      int c0 = (int)((int64_t)s * chromaRows / j.stripsTotal);
      int c1 = (int)((int64_t)(s + 1) * chromaRows / j.stripsTotal);
      for (int pl = 1; pl <= 2; pl++) {
        size_t off = pl == 1 ? sf.planeBytes[0]
                             : sf.planeBytes[0] + sf.planeBytes[1];
        for (int y = c0; y < c1; y++) {
          memcpy(j.staging.data() + off + (size_t)y * sf.refs.stride[pl],
                 sf.refs.p[pl] + (size_t)y * sf.refs.stride[pl],
                 (size_t)sf.refs.stride[pl]);
        }
      }
    }
  }

  bool encodeFrom(uint8_t* const planes[3], int surfaceIdx, uint64_t seq,
                  int64_t ptsUs, std::string& err) {
    SSourcePicture pic;
    memset(&pic, 0, sizeof pic);
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = fmt_.visibleW;
    pic.iPicHeight = fmt_.visibleH;
    pic.iStride[0] = fmt_.stride0;
    pic.iStride[1] = fmt_.stride1;
    pic.iStride[2] = fmt_.stride2;
    pic.pData[0] = planes[0];
    pic.pData[1] = planes[1];
    pic.pData[2] = planes[2];
    pic.uiTimeStamp = ptsUs;
    SFrameBSInfo info;
    memset(&info, 0, sizeof info);
    int rv = enc_->EncodeFrame(&pic, &info);
    if (rv != cmResultSuccess) {
      err = "EncodeFrame returned " + std::to_string(rv);
      return false;
    }
    bool key = info.eFrameType == videoFrameTypeIDR ||
               info.eFrameType == videoFrameTypeI;
    EncoderEvent bs;
    bs.type = EncoderEvent::Type::BitstreamReady;
    bs.seq = seq;
    bs.surfaceIndex = surfaceIdx;
    bs.tUs = nowUs();
    bs.keyframe = key;
    for (int l = 0; l < info.iLayerNum; l++) {
      SLayerBSInfo& li = info.sLayerInfo[l];
      int off = 0;
      for (int n = 0; n < li.iNalCount; n++) {
        bs.data.insert(bs.data.end(), li.pBsBuf + off,
                       li.pBsBuf + off + li.pNalLengthInByte[n]);
        off += li.pNalLengthInByte[n];
      }
    }
    if (bs.data.empty()) {
      err = "encoder returned an empty access unit";
      return false;
    }
    ready_.push_back(std::move(bs));
    return true;
  }

  EncoderSettings s_;
  EncoderCaps caps_;
  SurfaceFormat fmt_;
  ISVCEncoder* enc_ = nullptr;
  std::vector<Surface> surfaces_;
  std::deque<EncoderEvent> ready_;
  std::deque<Job> jobs_;
  std::vector<uint8_t> config_;
  bool eos_ = false;
  bool eosDone_ = false;
};

}  // namespace

std::unique_ptr<IEncoder> makeOpenH264Encoder(const EncoderSettings& s,
                                              std::string&) {
  return std::unique_ptr<IEncoder>(new OpenH264Encoder(s));
}

const char* encoderKindName(EncoderKind k) {
  return k == EncoderKind::V4l2Hardware ? "v4l2" : "openh264";
}

EncoderCaps softwareCaps() {
  EncoderSettings s;
  OpenH264Encoder e(s);
  return e.caps();
}
