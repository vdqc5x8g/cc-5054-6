#include "encoder.h"

#include <wels/codec_api.h>

#include <deque>
#include <thread>

namespace {

struct InFlight {
  int surfaceIdx = -1;
  uint64_t seq = 0;
  int64_t readyUs = 0;  // when the backend is done with the surface
};

// Software reference encoder.
//
// Receipt semantics: the bitstream is complete when submit() returns, so the
// session can take it immediately. The input surface, however, is still owned
// by the backend until the deferred-release timer fires and a
// SurfaceReleased event is emitted. The delay is an artificial harness delay
// for exercising backpressure; it is reported as such and never as device
// latency.
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
        "SurfaceReleased 携带确切帧号；即使码流已同步就绪，也必须等到该事件"
        "之后才能重填表面。";
    caps_.receiptSemantics =
        "BitstreamReady 在 submit() 内同步产生；没有异步编码队列，这里的延迟"
        "是实验台定时器，不是设备时延。";
    caps_.maxPool = 8;
    caps_.inputFormats = {"I420"};
    caps_.notes.push_back(
        "软件编码器既作为可执行对照，也作为异步硬件回执的接口模型。");
    caps_.limitations.push_back(
        "软件编码：吞吐与时延都是 CPU 数值，不是设备测量。");
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
    for (int i = 0; i < count; i++) {
      Surface& sf = surfaces_[i];
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
    return true;
  }

  int surfaceCount() const override { return (int)surfaces_.size(); }
  Surface& surface(int i) override { return surfaces_[i]; }

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
    SSourcePicture pic;
    memset(&pic, 0, sizeof pic);
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = fmt_.visibleW;
    pic.iPicHeight = fmt_.visibleH;
    pic.iStride[0] = sf.refs.stride[0];
    pic.iStride[1] = sf.refs.stride[1];
    pic.iStride[2] = sf.refs.stride[2];
    pic.pData[0] = sf.refs.p[0];
    pic.pData[1] = sf.refs.p[1];
    pic.pData[2] = sf.refs.p[2];
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
    ready_.push_back(bs);
    sf.state = Surface::State::Queued;  // backend still owns it
    InFlight f;
    f.surfaceIdx = surfaceIdx;
    f.seq = seq;
    f.readyUs = nowUs() + (int64_t)s_.receiptLagMs * 1000;
    inflight_.push_back(f);
    return true;
  }

  int poll(std::vector<EncoderEvent>& out, int, std::string&) override {
    int64_t t = nowUs();
    while (!inflight_.empty() && inflight_.front().readyUs <= t) {
      InFlight& f = inflight_.front();
      if (f.surfaceIdx >= 0 && f.surfaceIdx < (int)surfaces_.size()) {
        Surface& sf = surfaces_[f.surfaceIdx];
        sf.state = Surface::State::Free;
        sf.reuseCount++;
        EncoderEvent ev;
        ev.type = EncoderEvent::Type::SurfaceReleased;
        ev.surfaceIndex = f.surfaceIdx;
        ev.seq = f.seq;
        ev.tUs = t;
        out.push_back(std::move(ev));
      }
      inflight_.pop_front();
    }
    while (!ready_.empty()) {
      out.push_back(std::move(ready_.front()));
      ready_.pop_front();
    }
    eosDone_ = eos_ && inflight_.empty() && ready_.empty();
    return (int)out.size();
  }

  bool requestEos(std::string&) override {
    eos_ = true;
    eosDone_ = inflight_.empty() && ready_.empty();
    return true;
  }
  bool eosComplete() const override { return eosDone_; }

  bool surfaceWritable(int idx) const override {
    if (idx < 0 || idx >= (int)surfaces_.size()) return false;
    return surfaces_[idx].state == Surface::State::Free;
  }

  std::vector<uint8_t> codecConfig() const override { return config_; }

  void shutdown() override {
    if (enc_) {
      enc_->Uninitialize();
      WelsDestroySVCEncoder(enc_);
      enc_ = nullptr;
    }
    surfaces_.clear();
    inflight_.clear();
    ready_.clear();
  }

 private:
  EncoderSettings s_;
  EncoderCaps caps_;
  SurfaceFormat fmt_;
  ISVCEncoder* enc_ = nullptr;
  std::vector<Surface> surfaces_;
  std::deque<EncoderEvent> ready_;
  std::deque<InFlight> inflight_;
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
