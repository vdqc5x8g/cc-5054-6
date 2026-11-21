#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

// Pixel layout of an encoder input surface. All plane offsets/strides below are
// honoured; the coded extent may be larger than the visible extent.
enum class PixFmt { I420 = 0, NV12 = 1 };
// Three-plane multiplanar variants are handled by the V4L2 backend through
// separate plane pointers; PixFmt still describes the sample geometry.

const char* pixFmtName(PixFmt f);
bool pixFmtFromName(const std::string& name, PixFmt& out);

struct ImageLayout {
  PixFmt fmt = PixFmt::I420;
  int visibleW = 0, visibleH = 0;  // effective picture size
  int codedW = 0, codedH = 0;      // extent that is actually allocated
  int yStride = 0;                 // bytes per luma row
  int uvStride = 0;                // bytes per chroma row (I420) / UV row (NV12)
  int uvRows = 0;                  // allocated chroma rows
  size_t totalBytes = 0;
};

// Pointers and strides of a concrete surface's planes. For NV12 p[1] is the
// interleaved CbCr plane and p[2] is null.
struct PlaneRefs {
  uint8_t* p[3] = {nullptr, nullptr, nullptr};
  int stride[3] = {0, 0, 0};
};

ImageLayout makeLayout(PixFmt fmt, int visibleW, int visibleH, int strideAlign,
                       int codedAlign);

// One generated picture in packed RGB (visible extent only).
struct FramePlan {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;
  uint16_t marker = 0;
  uint8_t patchLevels[3] = {0, 0, 0};  // quantised luma levels of ref patches
  bool corruptionInjected = false;
};

constexpr int kMarkerBits = 12;

// Renders the synthetic test picture for `seq`: moving gradient, fixed colour
// reference patches and a frame-number marker strip.
FramePlan renderFrame(uint64_t seq, int width, int height,
                      bool injectCorruption);

// Real RGB -> YUV (BT.601 limited range) conversion with 2x2 chroma averaging,
// writing through the supplied strides. Everything between the visible extent
// and the allocated extent (columns up to the plane stride) is filled by edge
// replication so the encoder never reads uninitialised memory. Writes are
// clipped to the real per-plane byte sizes reported by the backend.
void convertRgbToSurface(const FramePlan& plan, const ImageLayout& layout,
                         const PlaneRefs& planes, const size_t planeBytes[3]);

struct YuvView {
  const uint8_t* plane[3] = {nullptr, nullptr, nullptr};
  int stride[3] = {0, 0, 0};
  int width = 0, height = 0;
  PixFmt fmt = PixFmt::I420;
};

// Converts a decoded frame (visible extent, decoder's own strides) back to RGB.
std::vector<uint8_t> yuvToRgb(const YuvView& view);

struct MarkerReading {
  uint16_t bits = 0;
  int blocks = 0;
  int confidence = 0;  // sum of |sample-128| over marker blocks
};
MarkerReading readMarker(const YuvView& view);
int readPatchLevels(const YuvView& view, uint8_t out[3]);

// Per-frame unique corner tags: the frame number (low 12 bits) is drawn as
// four spatially separated bit grids. If a surface is refilled while the
// encoder is still reading it, the decoded picture tears and the four tags
// disagree -- that is the identifiable frame-mixing signature.
struct CornerReading {
  uint16_t tag[4] = {0, 0, 0, 0};
  bool valid[4] = {false, false, false, false};
  int validCount = 0;
  bool agree = false;  // every readable tag holds the same value
  bool mixed = false;  // >=2 readable tags and they disagree (tearing)
};
CornerReading readCornerTags(const YuvView& view);

struct Thumb {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;
};
Thumb downscaleRgb(const uint8_t* rgb, int w, int h, int maxWidth);

double meanAbsError(const Thumb& a, const Thumb& b);
