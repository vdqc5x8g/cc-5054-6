#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
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
  std::string mode = "paced";  // paced | burst | late | guard | corrupt | earlyreuse
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
  // Surface scheduling: "fixed" pool or "elastic" grow/shrink on feedback.
  std::string sched = "fixed";
  int poolMin = 2;
  int poolMax = 8;
  // Deferred-read harness timers (software backend only).
  int readDelayMs = 0;
  int scanMs = 0;
  // Event gate: stepMode = manual single-step; order = auto preference when
  // both kinds are pending ("any" | "packet" | "return"); script replays a
  // recorded schedule ('p' = apply packet event, 'r' = apply return event).
  bool step = false;
  std::string order = "any";
  std::string script;
  // Resolution switch: at frame switchAt the old generation keeps draining
  // while a new generation is created at switchWidth x switchHeight.
  int switchAt = -1;
  int switchWidth = 0, switchHeight = 0;
};

struct SurfaceView {
  int index = 0;
  int gen = 0;
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
  int gen = 0;
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
  uint16_t cornerExpected = 0;
  uint16_t cornerTags[4] = {0, 0, 0, 0};
  int cornerValid = 0;
  bool cornersOk = false;
  bool cornersMixed = false;
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
  int cornerMismatches = 0, mixedFrames = 0;
  int ownershipViolations = 0;
  int staleGenReleases = 0;
  int backpressureStalls = 0;
  double stallMsTotal = 0;
  int maxInFlight = 0;
  int guardRejections = 0;
  int guardChecks = 0;
  int grows = 0, shrinks = 0, schedWaits = 0;
  int peakActiveSurfaces = 0;
  size_t peakMappedBytes = 0;
  int peakRssKb = 0, baselineRssKb = 0;
  size_t surfaceBytes = 0;
  int framesDrained = 0;
  double p50SubmitToAuMs = -1, maxSubmitToAuMs = -1;
  double p50SubmitToReleaseMs = -1, maxSubmitToReleaseMs = -1;
};

struct GenerationView {
  int id = 0;
  int width = 0, height = 0;
  std::string state;  // active | draining | done
  int surfaces = 0;
  int freeSurfaces = 0;
  size_t bytes = 0;
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
  std::string capacityHint;
  std::string timingSource = "simulated";  // simulated | device
  bool eosRequested = false, eosComplete = false;
  bool decoderOpen = false;
  bool verifyEnabled = false;
  bool encoderClosed = false;
  int framesDrained = 0;
  int leakedSurfaces = 0;
  size_t mappedBytes = 0;
  // Event gate state for the stepper view.
  bool stepMode = false;
  int pendingPackets = 0;
  int pendingReturns = 0;
  size_t scriptPos = 0, scriptLen = 0;
  std::string trace;
  std::vector<SurfaceView> surfaces;
  std::vector<GenerationView> generations;
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

  // Event gate controls (called from the HTTP thread).
  bool stepEvents(const std::string& kind, int n, std::string& err);
  void continueRun();

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

  // One encoder generation. A resolution switch keeps the old generation
  // alive (draining) while the new one is already accepting frames; surface
  // releases are routed by generation so a stale release can never free a
  // same-numbered surface of the new pool.
  struct Generation {
    int id = 0;
    std::unique_ptr<IEncoder> enc;
    EncoderCaps caps;
    SurfaceFormat format;
    ImageLayout layout;
    H264Decoder dec;
    bool decoderOpen = false;
    int width = 0, height = 0;
    bool eosRequested = false;
    bool closed = false;
    std::vector<int64_t> idleSinceUs;  // per-surface, for the shrink rule
  };

  struct PendingEv {
    EncoderEvent ev;
    int gen = 0;
  };

  void workerMain();
  bool makeGeneration(int width, int height, std::string& err);
  void switchGeneration();
  void closeGeneration(Generation& g);
  Generation& cur() { return *gens_.back(); }
  void pumpGeneration(Generation& g);
  bool pumpAndGate();  // pump all generations, then apply the gate
  int applyGate();     // returns events applied
  void applyEvent(PendingEv& pe);
  void handleBitstream(EncoderEvent& ev, Generation& g, int64_t now);
  void handleRelease(EncoderEvent& ev, Generation& g, int64_t now);
  bool acquireSurface(Generation& g, int& idx, int64_t pacingDeadline);
  void maybeShrink(Generation& g);
  double expectedOldestWaitMsLocked(Generation& g);
  void maybeInjectOverwrite(int64_t now);
  void updatePeaks();
  void logLine(const std::string& s);
  void finishRun(const std::string& status, const std::string& error);
  void finishRun(const std::string& status, const std::string& error,
                 int64_t endedUs);
  FrameView* frameFor(uint64_t seq);
  void pruneArtifacts();
  void refreshSurfacesLocked();
  bool quiescent() const;

  mutable std::mutex mu_;
  SessionConfig cfg_;
  std::vector<std::unique_ptr<Generation>> gens_;
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

  // Event gate.
  std::deque<PendingEv> pending_;
  bool stepMode_ = false;
  std::deque<char> tokens_;  // 'p' | 'r' | 'a'
  size_t scriptPos_ = 0;
  std::string trace_;
  int64_t gateBlockedSince_ = -1;

  // Elastic scheduling bookkeeping.
  std::vector<double> relLagMs_;

  // earlyreuse fault injection: overwrite the victim's surface mid-scan.
  bool overwritePending_ = false;
  int64_t overwriteAtUs_ = 0;
  int overwriteSurface_ = -1;
  uint64_t overwritePlanSeq_ = 0;
  int overwriteGen_ = -1;
};
