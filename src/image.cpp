#include "image.h"

#include <cstring>

namespace {

constexpr int kPatchOrigins[3][2] = {
    {4, 4},   // brightness level 2 (170)
    {4, 36},  // brightness level 1 (85)
    {4, 68},  // brightness level 3 (255)
};

struct Rgb {
  int r, g, b;
};

void drawRect(std::vector<uint8_t>& rgb, int w, int h, int x0, int y0, int rw,
              int rh, Rgb c) {
  for (int y = y0; y < y0 + rh; y++) {
    if (y < 0 || y >= h) continue;
    for (int x = x0; x < x0 + rw; x++) {
      if (x < 0 || x >= w) continue;
      size_t i = ((size_t)y * w + x) * 3;
      rgb[i + 0] = (uint8_t)c.r;
      rgb[i + 1] = (uint8_t)c.g;
      rgb[i + 2] = (uint8_t)c.b;
    }
  }
}

inline void rgbToYuv(int r, int g, int b, int& Y, int& U, int& V) {
  // BT.601 limited range, integer approximations.
  int y = (66 * r + 129 * g + 25 * b + 128) >> 8;
  Y = clamp8(y + 16);
  U = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
  V = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

}  // namespace

const char* pixFmtName(PixFmt f) {
  switch (f) {
    case PixFmt::I420: return "I420";
    case PixFmt::NV12: return "NV12";
  }
  return "?";
}

bool pixFmtFromName(const std::string& name, PixFmt& out) {
  if (name == "I420" || name == "i420") {
    out = PixFmt::I420;
    return true;
  }
  if (name == "NV12" || name == "nv12") {
    out = PixFmt::NV12;
    return true;
  }
  return false;
}

static int alignUp(int v, int a) { return ((v + a - 1) / a) * a; }

ImageLayout makeLayout(PixFmt fmt, int visibleW, int visibleH, int strideAlign,
                       int codedAlign) {
  ImageLayout l;
  l.fmt = fmt;
  l.visibleW = visibleW;
  l.visibleH = visibleH;
  l.codedW = alignUp(visibleW, codedAlign);
  l.codedH = alignUp(visibleH, codedAlign);
  l.yStride = alignUp(l.codedW, strideAlign);
  l.uvRows = l.codedH / 2;
  if (fmt == PixFmt::I420) {
    // Chroma planes store half-resolution samples side by side.
    l.uvStride = alignUp(l.codedW / 2, strideAlign);
    l.totalBytes = (size_t)l.yStride * l.codedH +
                   2 * (size_t)l.uvStride * l.uvRows;
  } else {
    l.uvStride = alignUp(l.codedW, strideAlign);
    l.totalBytes = (size_t)l.yStride * l.codedH +
                   (size_t)l.uvStride * l.uvRows;
  }
  return l;
}

FramePlan renderFrame(uint64_t seq, int width, int height,
                      bool injectCorruption) {
  FramePlan plan;
  plan.width = width;
  plan.height = height;
  plan.marker = (uint16_t)(seq & ((1u << kMarkerBits) - 1));
  plan.rgb.resize((size_t)width * height * 3);

  const double t = (double)seq;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      double u = (double)x / (double)std::max(1, width - 1);
      double v = (double)y / (double)std::max(1, height - 1);
      double r = 40 + 150 * u + 40 * std::sin(0.11 * t);
      double g = 40 + 120 * v + 40 * std::sin(0.07 * t + 1.3);
      double b = 90 + 80 * (1.0 - u) + 60 * std::cos(0.05 * t + 0.4);
      size_t i = ((size_t)y * width + x) * 3;
      plan.rgb[i + 0] = clamp8((int)(r + 0.5));
      plan.rgb[i + 1] = clamp8((int)(g + 0.5));
      plan.rgb[i + 2] = clamp8((int)(b + 0.5));
    }
  }
  // Moving diagonal band: ordering/tearing is visible frame to frame.
  int band = (int)((seq * 17) % (uint64_t)std::max(1, width));
  int thick = std::max(2, width / 90);
  for (int y = 0; y < height; y++) {
    int x0 = (band + y * 2) % std::max(1, width);
    for (int d = 0; d < thick; d++) {
      int x = (x0 + d) % std::max(1, width);
      size_t i = ((size_t)y * width + x) * 3;
      plan.rgb[i + 0] = 235;
      plan.rgb[i + 1] = 235;
      plan.rgb[i + 2] = 235;
    }
  }

  // Fixed reference patches in four luma levels (0/85/170/255).
  static const int levels[3] = {2, 1, 3};
  for (int p = 0; p < 3; p++) {
    int q = levels[p] * 85;
    plan.patchLevels[p] = (uint8_t)levels[p];
    drawRect(plan.rgb, width, height, kPatchOrigins[p][0], kPatchOrigins[p][1],
             24, 24, {q, q, q});
  }
  // Pure-ish primaries prove the colour matrix, not just luma.
  drawRect(plan.rgb, width, height, 4, 100, 24, 24, {255, 0, 0});
  drawRect(plan.rgb, width, height, 32, 100, 24, 24, {0, 255, 0});
  drawRect(plan.rgb, width, height, 60, 100, 24, 24, {0, 0, 255});

  // Frame-number marker: kMarkerBits blocks, bright = 1, dark = 0.
  const int blockH = std::min(28, std::max(8, height / 12));
  const int blockW = std::min(22, std::max(6, width / (kMarkerBits * 3)));
  const int startX = width - (blockW + 2) * kMarkerBits - 8;
  const int startY = height - blockH - 8;
  for (int bit = 0; bit < kMarkerBits; bit++) {
    bool one = (plan.marker >> bit) & 1u;
    int shade = one ? 235 : 16;
    drawRect(plan.rgb, width, height, startX + bit * (blockW + 2), startY,
             blockW, blockH, {shade, shade, shade});
  }

  if (injectCorruption && width > 2 && height > 2) {
    // Deliberately clobber the marker of the frame that is *encoded*, so a
    // frame/surface mix-up becomes observable after decoding.
    plan.corruptionInjected = true;
    for (int y = startY; y < startY + blockH; y++) {
      for (int x = startX; x < startX + (blockW + 2) * kMarkerBits && x < width;
           x++) {
        size_t i = ((size_t)y * width + x) * 3;
        plan.rgb[i + 0] = 255 - plan.rgb[i + 0];
        plan.rgb[i + 1] = 255 - plan.rgb[i + 1];
        plan.rgb[i + 2] = 255 - plan.rgb[i + 2];
      }
    }
  }
  return plan;
}

void convertRgbToSurface(const FramePlan& plan, const ImageLayout& layout,
                         const PlaneRefs& planes, const size_t planeBytes[3]) {
  const int vw = std::min(layout.visibleW, plan.width);
  const int vh = std::min(layout.visibleH, plan.height);
  const int codedH = layout.codedH;
  const int lumaRows =
      planeBytes && planeBytes[0] >= (size_t)planes.stride[0]
          ? (int)std::min<size_t>((size_t)codedH,
                                  planeBytes[0] / (size_t)planes.stride[0])
          : codedH;

  // Luma, then replicate the edge pixels across the coded padding so no
  // uninitialised bytes are handed to the encoder.
  for (int y = 0; y < lumaRows; y++) {
    uint8_t* row = planes.p[0] + (size_t)y * planes.stride[0];
    int sy = std::min(y, vh - 1);
    for (int x = 0; x < planes.stride[0]; x++) {
      int sx = std::min(x, vw - 1);
      size_t i = ((size_t)sy * plan.width + sx) * 3;
      int Y, U, V;
      rgbToYuv(plan.rgb[i], plan.rgb[i + 1], plan.rgb[i + 2], Y, U, V);
      row[x] = (uint8_t)Y;
    }
  }

  // Chroma with true 2x2 averaging over the visible extent. Columns/rows past
  // the visible area replicate the last visible block, again covering the
  // whole allocated stride.
  const int cStride = layout.fmt == PixFmt::I420 ? planes.stride[1]
                                                 : planes.stride[1];
  int cRows = layout.uvRows;
  size_t uvBytes = planeBytes ? planeBytes[1] : 0;
  if (layout.fmt == PixFmt::I420 && planeBytes && planeBytes[2] &&
      (uvBytes == 0 || planeBytes[2] < uvBytes))
    uvBytes = planeBytes[2];
  if (uvBytes && cStride > 0)
    cRows = (int)std::min<size_t>((size_t)cRows, uvBytes / (size_t)cStride);
  const int cCols = layout.fmt == PixFmt::I420 ? cStride : cStride / 2;
  for (int cy = 0; cy < cRows; cy++) {
    int by0 = std::min(cy * 2, vh - 1);
    int by1 = std::min(cy * 2 + 1, vh - 1);
    for (int cx = 0; cx < cCols; cx++) {
      int bx0 = std::min(cx * 2, vw - 1);
      int bx1 = std::min(cx * 2 + 1, vw - 1);
      int rr = 0, gg = 0, bb = 0;
      for (int dy = 0; dy < 2; dy++) {
        int sy = dy ? by1 : by0;
        for (int dx = 0; dx < 2; dx++) {
          int sx = dx ? bx1 : bx0;
          size_t i = ((size_t)sy * plan.width + sx) * 3;
          rr += plan.rgb[i];
          gg += plan.rgb[i + 1];
          bb += plan.rgb[i + 2];
        }
      }
      int Y, U, V;
      rgbToYuv(rr / 4, gg / 4, bb / 4, Y, U, V);
      if (layout.fmt == PixFmt::I420) {
        planes.p[1][(size_t)cy * planes.stride[1] + cx] = (uint8_t)U;
        planes.p[2][(size_t)cy * planes.stride[2] + cx] = (uint8_t)V;
      } else {
        uint8_t* p = planes.p[1] + (size_t)cy * planes.stride[1] + cx * 2;
        p[0] = (uint8_t)U;
        p[1] = (uint8_t)V;
      }
    }
  }
}

std::vector<uint8_t> yuvToRgb(const YuvView& view) {
  const int w = view.width, h = view.height;
  std::vector<uint8_t> rgb((size_t)w * h * 3);
  for (int y = 0; y < h; y++) {
    const uint8_t* yRow = view.plane[0] + (size_t)y * view.stride[0];
    const uint8_t* uRow = view.plane[1] + (size_t)(y / 2) * view.stride[1];
    const uint8_t* vRow = view.plane[2]
                              ? view.plane[2] + (size_t)(y / 2) * view.stride[2]
                              : nullptr;
    for (int x = 0; x < w; x++) {
      int Y = yRow[x];
      int U, V;
      if (view.fmt == PixFmt::NV12) {
        U = uRow[(x / 2) * 2];
        V = uRow[(x / 2) * 2 + 1];
      } else {
        U = uRow[x / 2];
        V = vRow ? vRow[x / 2] : 128;
      }
      int c = std::max(0, Y - 16);
      int d = U - 128;
      int e = V - 128;
      int r = (298 * c + 409 * e + 128) >> 8;
      int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
      int b = (298 * c + 516 * d + 128) >> 8;
      size_t i = ((size_t)y * w + x) * 3;
      rgb[i + 0] = clamp8(r);
      rgb[i + 1] = clamp8(g);
      rgb[i + 2] = clamp8(b);
    }
  }
  return rgb;
}

MarkerReading readMarker(const YuvView& view) {
  MarkerReading out;
  const int w = view.width, h = view.height;
  const int blockH = std::min(28, std::max(8, h / 12));
  const int blockW = std::min(22, std::max(6, w / (kMarkerBits * 3)));
  const int startX = w - (blockW + 2) * kMarkerBits - 8;
  const int startY = h - blockH - 8;
  for (int bit = 0; bit < kMarkerBits; bit++) {
    int x0 = startX + bit * (blockW + 2);
    int x1 = std::min(w, x0 + blockW);
    int y0 = startY;
    int y1 = std::min(h, y0 + blockH);
    if (x0 < 0 || x1 - x0 < 4 || y1 - y0 < 4) continue;
    long sum = 0, n = 0;
    for (int y = y0 + 2; y < y1 - 2; y += 2) {
      const uint8_t* row = view.plane[0] + (size_t)y * view.stride[0];
      for (int x = x0 + 2; x < x1 - 2; x += 2) {
        sum += row[x];
        n++;
      }
    }
    if (n == 0) continue;
    int avg = (int)(sum / n);
    out.confidence += std::abs(avg - 128);
    if (avg > 128) out.bits |= (uint16_t)(1u << bit);
    out.blocks++;
  }
  return out;
}

int readPatchLevels(const YuvView& view, uint8_t out[3]) {
  static const int origins[3][2] = {{4, 4}, {4, 36}, {4, 68}};
  int got = 0;
  for (int p = 0; p < 3; p++) {
    int x0 = origins[p][0] + 8, y0 = origins[p][1] + 8;
    if (x0 + 4 >= view.width || y0 + 4 >= view.height) {
      out[p] = 255;
      continue;
    }
    long sum = 0;
    int n = 0;
    for (int y = y0; y < y0 + 4; y++) {
      const uint8_t* row = view.plane[0] + (size_t)y * view.stride[0];
      for (int x = x0; x < x0 + 4; x++) {
        sum += row[x];
        n++;
      }
    }
    int avg = (int)(sum / std::max(1, n));
    int level = (avg * 3 + 127) / 255;
    out[p] = (uint8_t)std::min(3, std::max(0, level));
    got++;
  }
  return got;
}

Thumb downscaleRgb(const uint8_t* rgb, int w, int h, int maxWidth) {
  Thumb t;
  if (w <= 0 || h <= 0) return t;
  int tw = std::min(w, maxWidth);
  int th = std::max(1, (int)((int64_t)h * tw / w));
  t.width = tw;
  t.height = th;
  t.rgb.assign((size_t)tw * th * 3, 0);
  for (int ty = 0; ty < th; ty++) {
    int sy0 = (int)((int64_t)ty * h / th);
    int sy1 = std::max(sy0 + 1, (int)((int64_t)(ty + 1) * h / th));
    for (int tx = 0; tx < tw; tx++) {
      int sx0 = (int)((int64_t)tx * w / tw);
      int sx1 = std::max(sx0 + 1, (int)((int64_t)(tx + 1) * w / tw));
      long acc[3] = {0, 0, 0};
      int n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          size_t i = ((size_t)sy * w + sx) * 3;
          acc[0] += rgb[i];
          acc[1] += rgb[i + 1];
          acc[2] += rgb[i + 2];
          n++;
        }
      }
      size_t o = ((size_t)ty * tw + tx) * 3;
      t.rgb[o + 0] = (uint8_t)(acc[0] / n);
      t.rgb[o + 1] = (uint8_t)(acc[1] / n);
      t.rgb[o + 2] = (uint8_t)(acc[2] / n);
    }
  }
  return t;
}

double meanAbsError(const Thumb& a, const Thumb& b) {
  if (a.width != b.width || a.height != b.height || a.rgb.empty()) return 1e9;
  long long acc = 0;
  for (size_t i = 0; i < a.rgb.size(); i++)
    acc += std::abs((int)a.rgb[i] - (int)b.rgb[i]);
  return (double)acc / (double)a.rgb.size();
}
