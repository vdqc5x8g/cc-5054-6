#include "png.h"

#include <zlib.h>

#include <cstring>

namespace {

void be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v >> 24));
  out.push_back((uint8_t)(v >> 16));
  out.push_back((uint8_t)(v >> 8));
  out.push_back((uint8_t)v);
}

void chunk(std::vector<uint8_t>& out, const char* type,
           const std::vector<uint8_t>& data) {
  be32(out, (uint32_t)data.size());
  size_t typePos = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, out.data() + typePos, (uInt)(4 + data.size()));
  be32(out, (uint32_t)crc);
}

}  // namespace

std::vector<uint8_t> encodePngRgb(const uint8_t* rgb, int width, int height) {
  const size_t rowBytes = (size_t)width * 3;
  std::vector<uint8_t> raw;
  raw.reserve((rowBytes + 1) * height);
  for (int y = 0; y < height; y++) {
    raw.push_back(0);  // filter: none
    raw.insert(raw.end(), rgb + (size_t)y * rowBytes,
               rgb + (size_t)(y + 1) * rowBytes);
  }
  uLongf compLen = compressBound((uLong)raw.size());
  std::vector<uint8_t> comp(compLen);
  if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) != Z_OK)
    return {};
  comp.resize(compLen);

  std::vector<uint8_t> ihdr;
  be32(ihdr, (uint32_t)width);
  be32(ihdr, (uint32_t)height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: truecolour
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);

  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", comp);
  chunk(out, "IEND", {});
  return out;
}
