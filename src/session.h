#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "image.h"
#include "decoder.h"
#include "encoder.h"

struct SessionConfig {
  std::string backend = "openh264";  // "openh264" | "v4l2"
  std::string mode = "paced";        // paced | burst | late | guard | corrupt
  int width = 640, height = 360;
  int fps = 30;
  int bitrate = 3000000;
  int gop = 30;
  int pool = 4;
  int frames = 60;
  bool verify = true;
  int receiptLagMs = 0;
  std::string device;
  std::string format;
};

struct SurfaceView {
  int index = 0;
  std::string state;
  uint64_t seq = UINT64_MAX;
  int reuse = 0;
  int fills = 0;
  size_t bytes = 0;
  bool deviceMemory = false;
  std::string fmt;
  int visibleW = 0, visibleH = 0, codedW = 0, codedH = 0;
  int stride0 = 0, stride1 = 0, stride2 = 0;
  std::string releaseCond;
};

struct FrameView {
  uint64_t seq = 0;
  int order = -1;
  int surfaceIndex = -1;
  std::string status = "pending";  // pending | ok | fail | skipped | lost
  bool accepted = false;
  bool bitstreamReady = false;
  bool surfaceReusable = false;
  uint16_t markerExpected = 0, markerDecoded = 0;
  bool markerOk = false;
  bool patchesOk = false;
  bool colorsOk = false;
  bool decodeClean = false;
  bool corrupted = false;
  bool keyframe = false;
  int auBytes = 0;
  double meanAbsError = -1;
  double submitToAuMs = -1;
  double submitToReleaseMs = -1;
  int decodedW = 0, decodedH = 0;
  bool hasSrc = false, hasOut = false;
  std::string note;
  uint32_t auCrc = 0;
};

struct RunMetrics {
  int submitted = 0, accepted = 0, bitstreams = 0, released = 0;
  int verified = 0, failed = 0, skipped = 0, lost = 0, detected = 0;
  int outOfOrder = 0, duplicateSeq = 0, orphanAu = 0;
  int decodeErrors = 0, markerMismatches = 0, patchMismatches = 0;
  int colorMismatches = 0, pictureMismatches = 0;
  int backpressureStalls = 0;
  double stallMsTotal = 0;
  int maxInFlight = 0;
  int guardRejections = 0;
  int guardChecks = 0;
  int peakRssKb = 0, baselineRssKb = 0;
  size_t surfaceBytes = 0;
  int framesDrained = 0;
  double p50SubmitToAuMs = -1, maxSubmitToAuMs = -1;
  double p50SubmitToReleaseMs = -1, maxSubmitToReleaseMs = -1;
};

struct SessionSnapshot {
  std::string status = "idle";  // idle|starting|running|draining|completed|failed|stopped
  std::string error;
  std::string phase;
  SessionConfig cfg;
  EncoderCaps caps;
  SurfaceFormat format;
  bool sessionCreated = false;
  bool pipelineVerified = false;
  bool hardwareCovered = false;
  std::string verificationNote;
  bool eosRequested = false, eosComplete = false;
  bool decoderOpen = false;
  bool verifyEnabled = false;
  bool encoderClosed = false;
  int framesDrained = 0;
  int leakedSurfaces = 0;
  size_t mappedBytes = 0;
  std::vector<SurfaceView> surfaces;
  std::vector<FrameView> frames;
  RunMetrics metrics;
  std::vector<std::string> log;
  uint64_t version = 0;
  uint64_t latestSeq = UINT64_MAX;
  int64_t startedAtUs = 0, endedAtUs = 0;
};

class Session {
 public:
  Session();
  ~Session();

  bool start(const SessionConfig& cfg, std::string& err);
  void stop();
  bool join(int timeoutMs);
  bool running() const { return worker_.joinable(); }

  SessionSnapshot snapshot() const;
  // kind: "src" | "out". seq == UINT64_MAX means latest available.
  bool framePng(const std::string& kind, uint64_t seq, int maxWidth,
                std::vector<uint8_t>& png) const;
  bool accessUnit(uint64_t seq, std::vector<uint8_t>& data,
                  bool& keyframe) const;

 private:
  struct Artifacts {
    Thumb src, out;
    std::vector<uint8_t> au;
    bool keyframe = false;
  };

  void workerMain();
  void processEvents(std::vector<EncoderEvent>& evs, int64_t now);
  void handleBitstream(EncoderEvent& ev, int64_t now);
  void handleRelease(EncoderEvent& ev, int64_t now);
  bool acquireSurface(int& idx, int64_t pacingDeadline);
  void logLine(const std::string& s);
  void finishRun(const std::string& status, const std::string& error);
  void finishRun(const std::string& status, const std::string& error,
                 int64_t endedUs);
  void closeEncoder();
  FrameView* frameFor(uint64_t seq);
  void pruneArtifacts();
  void refreshSurfacesLocked();
  mutable std::mutex mu_;
  SessionConfig cfg_;
  EncoderCaps caps_;
  SurfaceFormat format_;
  std::unique_ptr<IEncoder> enc_;
  H264Decoder dec_;
  std::thread worker_;
  std::atomic<bool> stopFlag_{false};
  std::atomic<bool> workerDone_{false};
  std::atomic<bool> stopAfterFrame_{false};
  SessionSnapshot snap_;
  std::vector<FrameView> frames_;
  std::map<uint64_t, Artifacts> artifacts_;
  std::vector<uint64_t> artifactOrder_;
  RunMetrics m_;
  std::vector<double> latAu_, latRel_;
  std::unordered_map<uint64_t, int64_t> submitUs_;
  std::vector<SurfaceView> finalSurfaces_;
  int maxInFlightObserved_ = 0;
  int64_t runStartUs_ = 0;
  int64_t lastSeqArrival_ = -1;
  uint64_t lastLatencySeq_ = 0;
  bool haveLatencySeq_ = false;
  std::string fatal_;
  ImageLayout layout_;
};
