#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using SteadyClock = std::chrono::steady_clock;

inline int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

inline void sleepUs(int64_t us) {
  if (us > 0) std::this_thread::sleep_for(std::chrono::microseconds(us));
}

inline uint8_t clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

size_t processRssKb();

inline std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

// Minimal JSON writer. Arrays are built by joining serialized values.
class J {
 public:
  J() { s_ = "{"; }

  J& str(const char* k, const std::string& v) {
    sep();
    s_ += quote(k) + ":" + quote(v);
    return *this;
  }
  J& str(const char* k, const char* v) { return str(k, std::string(v)); }
  J& num(const char* k, long long v) {
    sep();
    s_ += quote(k) + ":" + std::to_string(v);
    return *this;
  }
  J& unum(const char* k, unsigned long long v) {
    sep();
    s_ += quote(k) + ":" + std::to_string(v);
    return *this;
  }
  J& dbl(const char* k, double v, int prec = 2) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", prec, v);
    sep();
    s_ += quote(k) + ":" + buf;
    return *this;
  }
  J& raw(const char* k, const std::string& json) {
    sep();
    s_ += quote(k) + ":" + json;
    return *this;
  }
  J& b(const char* k, bool v) {
    sep();
    s_ += quote(k) + ":" + (v ? "true" : "false");
    return *this;
  }
  std::string done() { return s_ + "}"; }

  static std::string quote(const std::string& v) {
    return "\"" + jsonEscape(v) + "\"";
  }
  static std::string arr(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++) {
      if (i) out += ",";
      out += items[i];
    }
    return out + "]";
  }

 private:
  void sep() {
    if (s_.size() > 1) s_ += ",";
  }
  std::string s_;
};
