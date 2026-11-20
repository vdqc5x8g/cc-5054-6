#include "encoder.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <deque>
#include <set>

namespace {

std::string fourccStr(uint32_t f) {
  char s[5] = {(char)(f & 0xff), (char)((f >> 8) & 0xff),
               (char)((f >> 16) & 0xff), (char)((f >> 24) & 0xff), 0};
  for (int i = 0; i < 4; i++)
    if (s[i] < 32 || s[i] > 126) s[i] = '?';
  return std::string(s);
}

bool isRawFormat(uint32_t f) {
  return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_NV12M ||
         f == V4L2_PIX_FMT_YUV420 || f == V4L2_PIX_FMT_YUV420M;
}

int xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

std::string errnoStr(const char* what) {
  return std::string(what) + ": " + strerror(errno);
}

uint32_t capsOf(const v4l2_capability& c) {
  return c.device_caps ? c.device_caps : c.capabilities;
}

std::vector<uint32_t> enumFormats(int fd, uint32_t type) {
  std::vector<uint32_t> out;
  for (uint32_t i = 0; i < 32; i++) {
    v4l2_fmtdesc d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.index = i;
    if (xioctl(fd, VIDIOC_ENUM_FMT, &d) != 0) break;
    out.push_back(d.pixelformat);
  }
  return out;
}

bool formatAdvertised(const std::vector<uint32_t>& v, uint32_t f) {
  return std::find(v.begin(), v.end(), f) != v.end();
}

}  // namespace

std::vector<V4l2Probe> probeV4l2Devices(int, int) {
  std::vector<V4l2Probe> out;
  for (int i = 0; i < 64; i++) {
    std::string path = "/dev/video" + std::to_string(i);
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
      close(fd);
      continue;
    }
    V4l2Probe p;
    p.path = path;
    p.driver = (const char*)cap.driver;
    p.card = (const char*)cap.card;
    p.bus = (const char*)cap.bus_info;
    p.opened = true;
    uint32_t c = capsOf(cap);
    p.m2m = (c & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
    if (!p.m2m) {
      close(fd);
      continue;  // only list memory-to-memory codec nodes
    }
    auto capFmts =
        enumFormats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE);
    // Enumeration types differ for M2M: use the MPLANE types when advertised.
    bool mplane = (c & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    auto capFmts2 = enumFormats(fd, mplane
                                        ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                        : V4L2_BUF_TYPE_VIDEO_CAPTURE);
    auto outFmts2 = enumFormats(fd, mplane
                                        ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                                        : V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (capFmts2.empty()) capFmts2 = std::move(capFmts);
    p.captureFormats.clear();
    p.outputFormats.clear();
    for (uint32_t f : capFmts2) p.captureFormats.push_back(fourccStr(f));
    for (uint32_t f : outFmts2) p.outputFormats.push_back(fourccStr(f));
    p.h264Capture = formatAdvertised(capFmts2, V4L2_PIX_FMT_H264);
    p.maxPool = 8;
    if (!p.h264Capture) {
      p.note = "M2M 节点但没有 H.264 CAPTURE 输出（仅解码或其他编解码）";
    } else {
      bool rawOk = false;
      for (uint32_t f : outFmts2)
        if (isRawFormat(f)) rawOk = true;
      if (!rawOk) {
        p.h264Capture = false;
        p.note = "有 H.264 CAPTURE，但 OUTPUT 未声明原始 YUV 格式";
      } else {
        p.note = mplane ? "多平面 M2M 编码器" : "单平面 M2M 编码器";
      }
    }
    out.push_back(std::move(p));
    close(fd);
  }
  return out;
}

EncoderCaps capsFromProbe(const V4l2Probe& p, int probeW, int probeH) {
  EncoderCaps c;
  c.kind = EncoderKind::V4l2Hardware;
  c.backendId = "v4l2";
  c.displayName = "V4L2 memory-to-memory H.264 encoder (" + p.driver + ")";
  c.hardware = true;
  c.device = p.path;
  c.driver = p.driver;
  c.card = p.card;
  c.bus = p.bus;
  c.deferredRelease = true;
  c.maxPool = p.maxPool;
  c.releaseSemantics =
      "输入表面由驱动持有，直到 OUTPUT 队列 DQBUF 返回该缓冲才发出 "
      "SurfaceReleased；提前改写会与硬件读取竞争。";
  c.receiptSemantics =
      "CAPTURE 队列 DQBUF 返回访问单元时发出 BitstreamReady；若驱动复制时间戳"
      "则按时间戳回映帧号，否则按 FIFO 顺序映射并在日志中说明。";
  c.inputFormats = p.outputFormats;
  c.notes = {
      p.note,
      "格式通过 S_FMT 协商，步幅与 plane 大小取自 G_FMT；不对对齐作任何假设。",
      "结束排空使用 VIDIOC_ENCODER_CMD(V4L2_ENC_CMD_STOP) 与 CAPTURE 的 LAST 标志。",
  };
  c.limitations = {
      "需要 /dev/videoX 节点 advertise V4L2_CAP_VIDEO_M2M(_MPLANE)，且 CAPTURE "
      "支持 H.264、OUTPUT 支持原始 YUV 格式。",
  };
  (void)probeW;
  (void)probeH;
  return c;
}

namespace {

struct OutBinding {
  int bufferIndex = -1;
  uint32_t planes = 0;
  size_t lengths[VIDEO_MAX_PLANES] = {0};
  void* addr[VIDEO_MAX_PLANES] = {nullptr, nullptr, nullptr};
  bool queued = false;
  uint64_t heldSeq = UINT64_MAX;
  size_t heldBytes = 0;
};

struct CapBinding {
  int bufferIndex = -1;
  uint32_t planes = 0;
  size_t lengths[VIDEO_MAX_PLANES] = {0};
  void* addr[VIDEO_MAX_PLANES] = {nullptr, nullptr, nullptr};
  bool queued = false;
};

struct PendingSeq {
  uint64_t seq = 0;
  int64_t ptsUs = 0;
  int outBuf = -1;
};

class V4l2Encoder : public IEncoder {
 public:
  V4l2Encoder(const EncoderSettings& s) : s_(s) {}
  ~V4l2Encoder() override { shutdown(); }

  EncoderCaps& caps() override { return caps_; }
  const SurfaceFormat& format() const override { return fmt_; }

  bool createPool(int count, std::string& err) override {
    if (!openDevice(err)) return false;
    if (!negotiateOutput(err)) return false;
    if (!negotiateCapture(err)) return false;
    if (count < 2 || count > caps_.maxPool) {
      err = "pool size must be 2..8";
      return false;
    }
    if (!requestOutputBuffers(count, err)) return false;
    if (!requestCaptureBuffers(std::max(4, count + 2), err)) return false;
    if (!mapBuffers(err)) return false;
    if (!applyControls(err)) return false;
    if (!streamOn(err)) return false;
    tilesBuilt_ = true;
    return true;
  }

  int surfaceCount() const override { return (int)surfaces_.size(); }
  Surface& surface(int i) override { return surfaces_[i]; }

  bool submit(int surfaceIdx, uint64_t seq, int64_t ptsUs,
              std::string& err) override {
    if (!streaming_) {
      err = "V4L2 device not streaming";
      return false;
    }
    if (surfaceIdx < 0 || surfaceIdx >= (int)surfaceIdxToBuf_.size()) {
      err = "bad surface index";
      return false;
    }
    if (surfaces_[surfaceIdx].state != Surface::State::Queued) {
      err = "surface not marked QUEUED before submit";
      return false;
    }
    int bi = surfaceIdxToBuf_[surfaceIdx];
    OutBinding& b = outBufs_[bi];
    if (b.queued) {
      err = "OUTPUT buffer still queued in driver";
      return false;
    }
    v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof planes);
    buf.type = outType_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (uint32_t)bi;
    buf.timestamp.tv_sec = ptsUs / 1000000;
    buf.timestamp.tv_usec = ptsUs % 1000000;
    buf.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
    if (mplane_) {
      buf.m.planes = planes;
      buf.length = b.planes;
      for (uint32_t p = 0; p < b.planes; p++) {
        planes[p].length = (uint32_t)b.lengths[p];
        planes[p].bytesused = (uint32_t)b.lengths[p];
      }
    }
    if (xioctl(fd_, VIDIOC_QBUF, &buf) != 0) {
      err = errnoStr("VIDIOC_QBUF(OUTPUT)");
      return false;
    }
    b.queued = true;
    b.heldSeq = seq;
    b.heldBytes = 0;
    PendingSeq ps;
    ps.seq = seq;
    ps.ptsUs = ptsUs;
    ps.outBuf = bi;
    pending_.push_back(ps);
    queuedOutputs_++;
    return true;
  }

  int poll(std::vector<EncoderEvent>& out, int timeoutMs, std::string& err) override {
    if (!fd_) return 0;
    if (!streaming_) return 0;
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN | POLLOUT;
    pfd.revents = 0;
    int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr < 0) {
      if (errno == EINTR) return 0;
      err = errnoStr("poll");
      return -1;
    }
    int n = 0;
    n += dequeueCapture(out);
    n += dequeueOutput(out);
    // Drain whatever else is immediately ready.
    for (;;) {
      int a = dequeueCapture(out);
      int b = dequeueOutput(out);
      if (a == 0 && b == 0) break;
      if (a < 0 || b < 0) break;
      n += a + b;
    }
    if (!err_.empty() && !fatalSent_) {
      EncoderEvent ev;
      ev.type = EncoderEvent::Type::Fatal;
      ev.detail = err_;
      ev.tUs = nowUs();
      out.push_back(ev);
      fatalSent_ = true;
      n++;
    }
    return n;
  }

  bool requestEos(std::string& err) override {
    if (eosRequested_) return true;
    v4l2_encoder_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cmd = V4L2_ENC_CMD_STOP;
    if (xioctl(fd_, VIDIOC_ENCODER_CMD, &cmd) != 0) {
      drainSupported_ = false;
      caps_.notes.push_back(
          "驱动拒绝了 VIDIOC_ENCODER_CMD(STOP)；排空可能收不到 CAPTURE LAST 标志。");
      err = errnoStr("VIDIOC_ENCODER_CMD(STOP)");
      eosRequested_ = true;
      return false;
    }
    drainSupported_ = true;
    eosRequested_ = true;
    return true;
  }

  bool eosComplete() const override {
    if (!eosRequested_) return false;
    return capEosSeen_ && queuedOutputs_ == 0;
  }
  bool eosSupported() const override { return drainSupported_; }

  bool surfaceWritable(int idx) const override {
    if (idx < 0 || idx >= (int)surfaceIdxToBuf_.size()) return false;
    int bi = surfaceIdxToBuf_[idx];
    if (bi < 0 || bi >= (int)outBufs_.size()) return false;
    return !outBufs_[bi].queued;
  }

  void shutdown() override {
    if (fd_ >= 0) {
      if (streaming_) {
        enum v4l2_buf_type t;
        t = outType_;
        xioctl(fd_, VIDIOC_STREAMOFF, &t);
        t = capType_;
        xioctl(fd_, VIDIOC_STREAMOFF, &t);
        streaming_ = false;
      }
      for (auto& b : outBufs_)
        for (uint32_t p = 0; p < b.planes; p++)
          if (b.addr[p]) munmap(b.addr[p], b.lengths[p]);
      for (auto& b : capBufs_)
        for (uint32_t p = 0; p < b.planes; p++)
          if (b.addr[p]) munmap(b.addr[p], b.lengths[p]);
      close(fd_);
      fd_ = -1;
    }
    outBufs_.clear();
    capBufs_.clear();
    surfaces_.clear();
    tilesBuilt_ = false;
  }

 private:
  bool openDevice(std::string& err) {
    std::string path = s_.device;
    if (path.empty()) {
      for (auto& p : probeV4l2Devices(s_.width, s_.height)) {
        if (p.h264Capture) {
          path = p.path;
          break;
        }
      }
    }
    if (path.empty()) {
      err = "在 /dev/video* 下未找到 V4L2 M2M H.264 编码节点";
      return false;
    }
    fd_ = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      err = errnoStr(("open " + path).c_str());
      return false;
    }
    v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
      err = errnoStr("VIDIOC_QUERYCAP");
      return false;
    }
    uint32_t c = capsOf(cap);
    bool m2m = (c & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
    if (!m2m) {
      err = path + " 不是 memory-to-memory 编解码节点";
      return false;
    }
    mplane_ = (c & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    outType_ = mplane_ ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                       : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    capType_ = mplane_ ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    auto capsFmts = enumFormats(fd_, capType_);
    if (!formatAdvertised(capsFmts, V4L2_PIX_FMT_H264)) {
      err = path + " 的 CAPTURE 队列未声明 H.264";
      return false;
    }
    devicesPath_ = path;
    caps_ = capsFromProbe(V4l2Probe{path, (const char*)cap.driver,
                                    (const char*)cap.card,
                                    (const char*)cap.bus_info, true, true, true,
                                    8, {}, {}, ""},
                          s_.width, s_.height);
    caps_.hardware = true;
    return true;
  }

  bool negotiateOutput(std::string& err) {
    auto fmts = enumFormats(fd_, outType_);
    std::vector<uint32_t> prefs;
    if (mplane_) {
      if (s_.format.empty() || s_.format == "NV12" || s_.format == "nv12")
        prefs.push_back(V4L2_PIX_FMT_NV12M);
      if (s_.format.empty() || s_.format == "I420" || s_.format == "i420")
        prefs.push_back(V4L2_PIX_FMT_YUV420M);
      if (s_.format.empty()) {
        prefs.push_back(V4L2_PIX_FMT_NV12);
        prefs.push_back(V4L2_PIX_FMT_YUV420);
      }
    } else {
      if (s_.format.empty() || s_.format == "NV12" || s_.format == "nv12")
        prefs.push_back(V4L2_PIX_FMT_NV12);
      if (s_.format.empty() || s_.format == "I420" || s_.format == "i420")
        prefs.push_back(V4L2_PIX_FMT_YUV420);
    }
    uint32_t chosen = 0;
    for (uint32_t f : prefs) {
      if (formatAdvertised(fmts, f)) {
        chosen = f;
        break;
      }
    }
    if (!chosen) {
      std::string list;
      for (uint32_t f : fmts) list += (list.empty() ? "" : ",") + fourccStr(f);
      err = "没有受支持的原始输出格式；驱动提供 [" + list +
            "]，请求为 '" + s_.format + "'";
      return false;
    }
    v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = outType_;
    if (mplane_) {
      f.fmt.pix_mp.width = (uint32_t)s_.width;
      f.fmt.pix_mp.height = (uint32_t)s_.height;
      f.fmt.pix_mp.pixelformat = chosen;
      f.fmt.pix_mp.field = V4L2_FIELD_NONE;
      uint32_t np = (chosen == V4L2_PIX_FMT_NV12M) ? 2 : 3;
      f.fmt.pix_mp.num_planes = np;
    } else {
      f.fmt.pix.width = (uint32_t)s_.width;
      f.fmt.pix.height = (uint32_t)s_.height;
      f.fmt.pix.pixelformat = chosen;
      f.fmt.pix.field = V4L2_FIELD_NONE;
    }
    if (xioctl(fd_, VIDIOC_S_FMT, &f) != 0) {
      err = errnoStr("VIDIOC_S_FMT(OUTPUT)");
      return false;
    }
    v4l2_format g;
    memset(&g, 0, sizeof g);
    g.type = outType_;
    if (xioctl(fd_, VIDIOC_G_FMT, &g) != 0) {
      err = errnoStr("VIDIOC_G_FMT(OUTPUT)");
      return false;
    }
    fmt_.fmt = (chosen == V4L2_PIX_FMT_NV12 || chosen == V4L2_PIX_FMT_NV12M)
                   ? PixFmt::NV12
                   : PixFmt::I420;
    fmt_.pixTag = fourccStr(chosen);
    fmt_.requestedW = s_.width;
    fmt_.requestedH = s_.height;
    fmt_.visibleW = s_.width;
    fmt_.visibleH = s_.height;
    fmt_.codedW = mplane_ ? (int)g.fmt.pix_mp.width : (int)g.fmt.pix.width;
    fmt_.codedH = mplane_ ? (int)g.fmt.pix_mp.height : (int)g.fmt.pix.height;
    fmt_.grantedDiffers = fmt_.codedW != s_.width || fmt_.codedH != s_.height;
    fmt_.stride0 = mplane_ ? (int)g.fmt.pix_mp.plane_fmt[0].bytesperline
                           : (int)g.fmt.pix.bytesperline;
    if (fmt_.stride0 <= 0) fmt_.stride0 = fmt_.codedW;
    if (fmt_.fmt == PixFmt::NV12) {
      fmt_.stride1 = mplane_ && g.fmt.pix_mp.num_planes > 1
                         ? (int)g.fmt.pix_mp.plane_fmt[1].bytesperline
                         : fmt_.stride0;
      if (fmt_.stride1 <= 0) fmt_.stride1 = fmt_.stride0;
    } else {
      fmt_.stride1 = fmt_.stride0 / 2;
      fmt_.stride2 = fmt_.stride1;
      if (mplane_ && g.fmt.pix_mp.num_planes > 2) {
        int s1 = (int)g.fmt.pix_mp.plane_fmt[1].bytesperline;
        int s2 = (int)g.fmt.pix_mp.plane_fmt[2].bytesperline;
        if (s1 > 0) fmt_.stride1 = s1;
        if (s2 > 0) fmt_.stride2 = s2;
      }
    }
    numOutPlanes_ = mplane_ ? g.fmt.pix_mp.num_planes : 1;
    if (numOutPlanes_ == 0 || numOutPlanes_ > 3) {
      err = "驱动报告了 " + std::to_string(numOutPlanes_) + " 个输出平面";
      return false;
    }
    for (uint32_t i = 0; i < numOutPlanes_; i++)
      outPlaneSizes_[i] =
          mplane_ ? (size_t)g.fmt.pix_mp.plane_fmt[i].sizeimage
                  : (size_t)g.fmt.pix.sizeimage;
    caps_.notes.push_back("协商结果 " + fmt_.pixTag + " " +
                          std::to_string(fmt_.codedW) + "x" +
                          std::to_string(fmt_.codedH) + " 步幅 " +
                          std::to_string(fmt_.stride0) + "/" +
                          std::to_string(fmt_.stride1));
    return true;
  }

  bool negotiateCapture(std::string& err) {
    v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = capType_;
    if (mplane_) {
      f.fmt.pix_mp.width = (uint32_t)s_.width;
      f.fmt.pix_mp.height = (uint32_t)s_.height;
      f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
      f.fmt.pix_mp.field = V4L2_FIELD_NONE;
      f.fmt.pix_mp.num_planes = 1;
    } else {
      f.fmt.pix.width = (uint32_t)s_.width;
      f.fmt.pix.height = (uint32_t)s_.height;
      f.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
      f.fmt.pix.field = V4L2_FIELD_NONE;
    }
    if (xioctl(fd_, VIDIOC_S_FMT, &f) != 0) {
      err = errnoStr("VIDIOC_S_FMT(CAPTURE)");
      return false;
    }
    v4l2_format g;
    memset(&g, 0, sizeof g);
    g.type = capType_;
    if (xioctl(fd_, VIDIOC_G_FMT, &g) != 0) {
      err = errnoStr("VIDIOC_G_FMT(CAPTURE)");
      return false;
    }
    numCapPlanes_ = mplane_ ? g.fmt.pix_mp.num_planes : 1;
    for (uint32_t i = 0; i < numCapPlanes_; i++)
      capPlaneSizes_[i] =
          mplane_ ? (size_t)g.fmt.pix_mp.plane_fmt[i].sizeimage
                  : (size_t)g.fmt.pix.sizeimage;
    return true;
  }

  bool requestOutputBuffers(int count, std::string& err) {
    v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = (uint32_t)count;
    rb.type = outType_;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &rb) != 0) {
      err = errnoStr("VIDIOC_REQBUFS(OUTPUT)");
      return false;
    }
    if (rb.count < (uint32_t)count) {
      err = "驱动只授予 " + std::to_string(rb.count) + " 个输出缓冲（请求 " +
            std::to_string(count) + "）";
      return false;
    }
    outBufs_.resize(count);
    for (int i = 0; i < count; i++) outBufs_[i].bufferIndex = i;
    caps_.maxPool = count;
    return true;
  }

  bool requestCaptureBuffers(int count, std::string& err) {
    v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = (uint32_t)count;
    rb.type = capType_;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &rb) != 0) {
      err = errnoStr("VIDIOC_REQBUFS(CAPTURE)");
      return false;
    }
    capBufs_.resize(rb.count);
    for (uint32_t i = 0; i < rb.count; i++) capBufs_[i].bufferIndex = (int)i;
    return true;
  }

  bool mapBuffers(std::string& err) {
    // OUTPUT
    for (size_t bi = 0; bi < outBufs_.size(); bi++) {
      v4l2_buffer buf;
      memset(&buf, 0, sizeof buf);
      v4l2_plane planes[VIDEO_MAX_PLANES];
      memset(planes, 0, sizeof planes);
      buf.type = outType_;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = (uint32_t)bi;
      if (mplane_) {
        buf.m.planes = planes;
        buf.length = numOutPlanes_;
      }
      if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) != 0) {
        err = errnoStr("VIDIOC_QUERYBUF(OUTPUT)");
        return false;
      }
      OutBinding& b = outBufs_[bi];
      b.planes = mplane_ ? buf.length : 1;
      for (uint32_t p = 0; p < b.planes; p++) {
        b.lengths[p] = mplane_ ? planes[p].length : buf.length;
        b.addr[p] = mmap(nullptr, b.lengths[p], PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, (off_t)(mplane_ ? planes[p].m.mem_offset
                                                          : buf.m.offset));
        if (b.addr[p] == MAP_FAILED) {
          b.addr[p] = nullptr;
          err = errnoStr("mmap(OUTPUT)");
          return false;
        }
      }
    }
    // CAPTURE
    for (size_t bi = 0; bi < capBufs_.size(); bi++) {
      v4l2_buffer buf;
      memset(&buf, 0, sizeof buf);
      v4l2_plane planes[VIDEO_MAX_PLANES];
      memset(planes, 0, sizeof planes);
      buf.type = capType_;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = (uint32_t)bi;
      if (mplane_) {
        buf.m.planes = planes;
        buf.length = numCapPlanes_;
      }
      if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) != 0) {
        err = errnoStr("VIDIOC_QUERYBUF(CAPTURE)");
        return false;
      }
      CapBinding& b = capBufs_[bi];
      b.planes = mplane_ ? buf.length : 1;
      for (uint32_t p = 0; p < b.planes; p++) {
        b.lengths[p] = mplane_ ? planes[p].length : buf.length;
        b.addr[p] = mmap(nullptr, b.lengths[p], PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd_, (off_t)(mplane_ ? planes[p].m.mem_offset
                                                          : buf.m.offset));
        if (b.addr[p] == MAP_FAILED) {
          b.addr[p] = nullptr;
          err = errnoStr("mmap(CAPTURE)");
          return false;
        }
      }
    }
    // Build surfaces bound 1:1 to OUTPUT buffers.
    surfaces_.resize(outBufs_.size());
    surfaceIdxToBuf_.assign(outBufs_.size(), -1);
    for (size_t si = 0; si < surfaces_.size(); si++) {
      Surface& sf = surfaces_[si];
      sf.index = (int)si;
      sf.fmt = fmt_.fmt;
      sf.visibleW = fmt_.visibleW;
      sf.visibleH = fmt_.visibleH;
      sf.codedW = fmt_.codedW;
      sf.codedH = fmt_.codedH;
      sf.deviceMemory = true;
      sf.planeCount = (int)outBufs_[si].planes;
      OutBinding& b = outBufs_[si];
      if (fmt_.fmt == PixFmt::NV12 && b.planes >= 2) {
        sf.refs.p[0] = (uint8_t*)b.addr[0];
        sf.refs.p[1] = (uint8_t*)b.addr[1];
        sf.refs.p[2] = nullptr;
        sf.refs.stride[0] = fmt_.stride0;
        sf.refs.stride[1] = fmt_.stride1;
        sf.planeBytes[0] = b.lengths[0];
        sf.planeBytes[1] = b.lengths[1];
      } else if (fmt_.fmt == PixFmt::I420 && b.planes >= 3) {
        sf.refs.p[0] = (uint8_t*)b.addr[0];
        sf.refs.p[1] = (uint8_t*)b.addr[1];
        sf.refs.p[2] = (uint8_t*)b.addr[2];
        sf.refs.stride[0] = fmt_.stride0;
        sf.refs.stride[1] = fmt_.stride1;
        sf.refs.stride[2] = fmt_.stride2;
        sf.planeBytes[0] = b.lengths[0];
        sf.planeBytes[1] = b.lengths[1];
        sf.planeBytes[2] = b.lengths[2];
      } else if (b.planes == 1) {
        // Single-planar: derive plane offsets from the negotiated strides.
        uint8_t* base = (uint8_t*)b.addr[0];
        size_t yBytes = (size_t)fmt_.stride0 * fmt_.codedH;
        sf.refs.p[0] = base;
        sf.refs.stride[0] = fmt_.stride0;
        sf.planeBytes[0] = yBytes;
        if (fmt_.fmt == PixFmt::NV12) {
          sf.refs.p[1] = base + yBytes;
          sf.refs.stride[1] = fmt_.stride1;
          sf.planeBytes[1] = std::min(b.lengths[0] - yBytes,
                                      (size_t)fmt_.stride1 * (fmt_.codedH / 2));
          if (b.lengths[0] < yBytes + sf.planeBytes[1]) {
            err = "单平面 NV12 缓冲区小于协商步幅所需大小";
            return false;
          }
        } else {
          size_t uvBytes = (size_t)fmt_.stride1 * (fmt_.codedH / 2);
          sf.refs.p[1] = base + yBytes;
          sf.refs.p[2] = base + yBytes + uvBytes;
          sf.refs.stride[1] = fmt_.stride1;
          sf.refs.stride[2] = fmt_.stride2;
          sf.planeBytes[1] = uvBytes;
          sf.planeBytes[2] = uvBytes;
          if (b.lengths[0] < yBytes + 2 * uvBytes) {
            err = "单平面 YUV420 缓冲区小于协商步幅所需大小";
            return false;
          }
        }
      } else {
        err = "格式 " + fmt_.pixTag + " 出现意外平面数 " + std::to_string(b.planes);
        return false;
      }
      sf.totalBytes = sf.planeBytes[0] + sf.planeBytes[1] + sf.planeBytes[2];
      sf.state = Surface::State::Free;
      surfaceIdxToBuf_[si] = (int)si;
    }
    return true;
  }

  bool applyControls(std::string& err) {
    struct Ctl {
      uint32_t id;
      int32_t value;
      const char* name;
    };
    uint32_t profile = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
    std::vector<Ctl> wanted = {
        {V4L2_CID_MPEG_VIDEO_BITRATE, s_.bitrate, "bitrate"},
        {V4L2_CID_MPEG_VIDEO_GOP_SIZE, s_.gop, "gop"},
    };
    std::vector<std::string> applied;
    for (auto& c : wanted) {
      v4l2_queryctrl q;
      memset(&q, 0, sizeof q);
      q.id = c.id;
      if (xioctl(fd_, VIDIOC_QUERYCTRL, &q) == 0 &&
          !(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
        v4l2_control ctl;
        memset(&ctl, 0, sizeof ctl);
        ctl.id = c.id;
        ctl.value = c.value;
        if (xioctl(fd_, VIDIOC_S_CTRL, &ctl) == 0) {
          applied.push_back(c.name);
        } else {
          caps_.notes.push_back(std::string("控制项 ") + c.name +
                                " 被拒绝：" + strerror(errno));
        }
      } else {
        caps_.notes.push_back(std::string("驱动没有 ") + c.name + " 控制项");
      }
    }
    v4l2_queryctrl q;
    memset(&q, 0, sizeof q);
    q.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
    if (xioctl(fd_, VIDIOC_QUERYCTRL, &q) == 0 &&
        !(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
      v4l2_control ctl;
      memset(&ctl, 0, sizeof ctl);
      ctl.id = q.id;
      ctl.value = (int32_t)profile;
      v4l2_ext_control ec;
      memset(&ec, 0, sizeof ec);
      ec.id = q.id;
      ec.value = (int32_t)profile;
      v4l2_ext_controls ecs;
      memset(&ecs, 0, sizeof ecs);
      ecs.ctrl_class = V4L2_CTRL_CLASS_MPEG;
      ecs.count = 1;
      ecs.controls = &ec;
      if (xioctl(fd_, VIDIOC_S_EXT_CTRLS, &ecs) == 0) {
        applied.push_back("profile=baseline");
      } else if (xioctl(fd_, VIDIOC_S_CTRL, &ctl) == 0) {
        applied.push_back("profile");
      }
    }
    std::string s = "已应用控制项：";
    for (auto& a : applied) s += " " + a;
    if (applied.empty()) s += "（无，保持驱动默认值）";
    caps_.notes.push_back(s);

    // Baseline has no B-frames; if the driver only supports other profiles we
    // report it instead of silently reordering frames.
    return true;
  }

  bool streamOn(std::string& err) {
    for (auto& b : capBufs_) {
      if (!queueCapture(b, err)) return false;
    }
    enum v4l2_buf_type t = capType_;
    if (xioctl(fd_, VIDIOC_STREAMON, &t) != 0) {
      err = errnoStr("VIDIOC_STREAMON(CAPTURE)");
      return false;
    }
    t = outType_;
    if (xioctl(fd_, VIDIOC_STREAMON, &t) != 0) {
      err = errnoStr("VIDIOC_STREAMON(OUTPUT)");
      return false;
    }
    streaming_ = true;
    return true;
  }

  bool queueCapture(CapBinding& b, std::string& err) {
    v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof planes);
    buf.type = capType_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (uint32_t)b.bufferIndex;
    if (mplane_) {
      buf.m.planes = planes;
      buf.length = b.planes;
      for (uint32_t p = 0; p < b.planes; p++) {
        planes[p].length = (uint32_t)b.lengths[p];
        planes[p].bytesused = 0;
      }
    }
    if (xioctl(fd_, VIDIOC_QBUF, &buf) != 0) {
      err = errnoStr("VIDIOC_QBUF(CAPTURE)");
      return false;
    }
    b.queued = true;
    return true;
  }

  int dequeueCapture(std::vector<EncoderEvent>& out) {
    v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof planes);
    buf.type = capType_;
    buf.memory = V4L2_MEMORY_MMAP;
    if (mplane_) {
      buf.m.planes = planes;
      buf.length = numCapPlanes_;
    }
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) != 0) {
      if (errno == EAGAIN || errno == EINTR) return 0;
      err_ = errnoStr("VIDIOC_DQBUF(CAPTURE)");
      return 0;
    }
    if (buf.index >= capBufs_.size()) {
      err_ = "CAPTURE 缓冲索引越界";
      return 0;
    }
    CapBinding& b = capBufs_[buf.index];
    b.queued = false;
    size_t bytes = 0;
    if (mplane_) {
      for (uint32_t p = 0; p < buf.length; p++) bytes += planes[p].bytesused;
    } else {
      bytes = buf.bytesused;
    }
    bool key = (buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    bool errFlag = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    bool last = (buf.flags & V4L2_BUF_FLAG_LAST) != 0;

    EncoderEvent ev;
    ev.type = EncoderEvent::Type::BitstreamReady;
    ev.tUs = nowUs();
    ev.keyframe = key;
    ev.surfaceIndex = -1;
    ev.seq = matchSeq(buf.timestamp, ev.detail);
    if (errFlag) {
      ev.detail += " CAPTURE 缓冲被标记 ERROR；";
    }
    if (bytes > 0 && bytes < 64 * 1024 * 1024) {
      if (b.planes == 1) {
        ev.data.assign((uint8_t*)b.addr[0], (uint8_t*)b.addr[0] + bytes);
      } else {
        // Concatenate planes in order; H.264 capture is normally plane 0 only.
        for (uint32_t p = 0; p < buf.length; p++) {
          uint8_t* base = (uint8_t*)b.addr[p];
          size_t n = planes[p].bytesused;
          if (base && n) ev.data.insert(ev.data.end(), base, base + n);
        }
      }
    }
    if (!ev.data.empty()) out.push_back(std::move(ev));
    else if (last) {
      // No frame payload but LAST marks the end of the drain.
    } else {
      EncoderEvent empty;
      empty.type = EncoderEvent::Type::BitstreamReady;
      empty.tUs = ev.tUs;
      empty.seq = ev.seq;
      empty.detail = "empty CAPTURE buffer";
      out.push_back(std::move(empty));
    }

    if (last) {
      capEosSeen_ = true;
    }
    if (!eosRequested_ || !last) {
      std::string qerr;
      if (!queueCapture(b, qerr)) {
        err_ = qerr;
      }
    }
    return 1;
  }

  uint64_t matchSeq(const timeval& ts, std::string& detail) {
    int64_t pts = (int64_t)ts.tv_sec * 1000000 + ts.tv_usec;
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->ptsUs == pts) {
        uint64_t seq = it->seq;
        pending_.erase(it);
        return seq;
      }
    }
    if (!pending_.empty()) {
      uint64_t seq = pending_.front().seq;
      pending_.pop_front();
      if (!timestampFallbackNoted_) {
        timestampFallbackNoted_ = true;
        caps_.notes.push_back(
            "CAPTURE 时间戳与提交时间戳不匹配，帧号按 FIFO 顺序映射。");
      }
      detail = "时间戳未匹配，按 FIFO 映射";
      return seq;
    }
    detail = "CAPTURE 缓冲没有对应的待回执帧";
    return UINT64_MAX;
  }

  int dequeueOutput(std::vector<EncoderEvent>& out) {
    v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof planes);
    buf.type = outType_;
    buf.memory = V4L2_MEMORY_MMAP;
    if (mplane_) {
      buf.m.planes = planes;
      buf.length = numOutPlanes_;
    }
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) != 0) {
      if (errno == EAGAIN || errno == EINTR) return 0;
      err_ = errnoStr("VIDIOC_DQBUF(OUTPUT)");
      return 0;
    }
    if (buf.index >= outBufs_.size()) {
      err_ = "OUTPUT 缓冲索引越界";
      return 0;
    }
    OutBinding& b = outBufs_[buf.index];
    b.queued = false;
    queuedOutputs_--;
    int si = -1;
    for (size_t i = 0; i < surfaceIdxToBuf_.size(); i++)
      if (surfaceIdxToBuf_[i] == (int)buf.index) si = (int)i;
    if (si < 0) {
      err_ = "OUTPUT 缓冲未绑定到任何表面";
      return 0;
    }
    Surface& sf = surfaces_[si];
    EncoderEvent ev;
    ev.type = EncoderEvent::Type::SurfaceReleased;
    ev.surfaceIndex = si;
    ev.seq = b.heldSeq;
    ev.tUs = nowUs();
    if (buf.flags & V4L2_BUF_FLAG_ERROR)
      ev.detail = "OUTPUT 缓冲被标记 ERROR；";
    b.heldSeq = UINT64_MAX;
    if (sf.state == Surface::State::Queued) {
      sf.state = Surface::State::Free;
      sf.reuseCount++;
    }
    out.push_back(std::move(ev));
    if (eosRequested_ && queuedOutputs_ == 0) {
      // Nothing left in the driver on the input side.
    }
    return 1;
  }

  EncoderSettings s_;
  EncoderCaps caps_;
  SurfaceFormat fmt_;
  int fd_ = -1;
  bool mplane_ = false;
  bool streaming_ = false;
  bool tilesBuilt_ = false;
  v4l2_buf_type outType_ = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  v4l2_buf_type capType_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  uint32_t numOutPlanes_ = 0, numCapPlanes_ = 0;
  size_t outPlaneSizes_[VIDEO_MAX_PLANES] = {0, 0, 0};
  size_t capPlaneSizes_[VIDEO_MAX_PLANES] = {0, 0, 0};
  std::string devicesPath_;
  std::vector<OutBinding> outBufs_;
  std::vector<CapBinding> capBufs_;
  std::vector<Surface> surfaces_;
  std::vector<int> surfaceIdxToBuf_;
  std::deque<PendingSeq> pending_;
  std::string err_;
  bool fatalSent_ = false;
  bool eosRequested_ = false;
  bool capEosSeen_ = false;
  bool drainSupported_ = true;
  int queuedOutputs_ = 0;
  bool timestampFallbackNoted_ = false;
};

}  // namespace

std::unique_ptr<IEncoder> makeV4l2Encoder(const EncoderSettings& s,
                                          std::string& err) {
  auto e = std::unique_ptr<V4l2Encoder>(new V4l2Encoder(s));
  err.clear();
  return e;
}
