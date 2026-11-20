#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "image.h"

enum class EncoderKind { OpenH264Software, V4l2Hardware };

struct EncoderSettings {
  int width = 640;
  int height = 360;
  int fps = 30;
  int bitrate = 3000000;
  int gop = 30;
  int pool = 4;
  std::string format;      // empty = backend default
  std::string device;      // empty = first usable V4L2 node
  int receiptLagMs = 0;    // software harness only; never reported as device latency
};

struct EncoderCaps {
  EncoderKind kind = EncoderKind::OpenH264Software;
  std::string backendId;    // "v4l2" / "openh264"
  std::string displayName;
  std::string device, driver, card, bus;
  bool hardware = false;
  bool deferredRelease = false;
  std::string releaseSemantics;
  std::string receiptSemantics;
  int maxPool = 8;
  std::vector<std::string> inputFormats;
  std::vector<std::string> notes;
  std::vector<std::string> limitations;
  std::string probeError;
};

struct SurfaceFormat {
  PixFmt fmt = PixFmt::I420;
  std::string pixTag;  // negotiated fourcc name, e.g. "NV12M"
  int requestedW = 0, requestedH = 0;
  int visibleW = 0, visibleH = 0;
  int codedW = 0, codedH = 0;
  int stride0 = 0, stride1 = 0, stride2 = 0;
  bool grantedDiffers = false;
};

struct Surface {
  int index = -1;
  PixFmt fmt = PixFmt::I420;
  int visibleW = 0, visibleH = 0, codedW = 0, codedH = 0;
  PlaneRefs refs;
  std::vector<uint8_t> memory;  // backing store for host-memory surfaces
  int planeCount = 0;
  size_t planeBytes[3] = {0, 0, 0};
  size_t totalBytes = 0;
  bool deviceMemory = false;

  // Lifecycle. Mutated only by the session while it holds its lock.
  enum class State { Free, Filling, Queued, Retired };
  State state = State::Free;
  uint64_t heldSeq = UINT64_MAX;
  int reuseCount = 0;
  size_t fillCount = 0;

  const char* stateName() const {
    switch (state) {
      case State::Free: return "FREE";
      case State::Filling: return "FILLING";
      case State::Queued: return "QUEUED";
      case State::Retired: return "RETIRED";
    }
    return "?";
  }
};

struct EncoderEvent {
  enum class Type { SurfaceReleased, BitstreamReady, Fatal };
  Type type = Type::SurfaceReleased;
  int surfaceIndex = -1;
  uint64_t seq = UINT64_MAX;
  int64_t tUs = 0;
  bool keyframe = false;
  std::string detail;
  std::vector<uint8_t> data;
};

class IEncoder {
 public:
  virtual ~IEncoder() = default;
  virtual EncoderCaps& caps() = 0;
  virtual const SurfaceFormat& format() const = 0;
  virtual bool createPool(int count, std::string& err) = 0;
  virtual int surfaceCount() const = 0;
  virtual Surface& surface(int i) = 0;
  // The caller guarantees it no longer touches the surface after this call
  // until a SurfaceReleased event for it arrives.
  virtual bool submit(int surfaceIdx, uint64_t seq, int64_t ptsUs,
                      std::string& err) = 0;
  virtual int poll(std::vector<EncoderEvent>& out, int timeoutMs,
                   std::string& err) = 0;
  virtual bool requestEos(std::string& err) = 0;
  virtual bool eosComplete() const = 0;
  virtual bool eosSupported() const { return true; }
  // Backend-owned view of whether application code may still write this
  // surface. For V4L2 this is the actual driver queue state (buffer not yet
  // DQBUF'd); for the software backend it is the deferred-release state.
  virtual bool surfaceWritable(int idx) const = 0;
  virtual std::vector<uint8_t> codecConfig() const { return {}; }
  virtual void shutdown() = 0;
};

std::unique_ptr<IEncoder> makeOpenH264Encoder(const EncoderSettings& s,
                                              std::string& err);
std::unique_ptr<IEncoder> makeV4l2Encoder(const EncoderSettings& s,
                                          std::string& err);

struct V4l2Probe {
  std::string path, driver, card, bus;
  bool opened = false;
  bool m2m = false;
  bool h264Capture = false;
  int maxPool = 0;
  std::vector<std::string> outputFormats;
  std::vector<std::string> captureFormats;
  std::string note;
};

std::vector<V4l2Probe> probeV4l2Devices(int probeW, int probeH);
EncoderCaps softwareCaps();
EncoderCaps capsFromProbe(const V4l2Probe& p, int probeW, int probeH);

// Free-form lifecycle description shared by the UI and README.
const char* encoderKindName(EncoderKind k);
