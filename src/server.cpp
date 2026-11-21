#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <thread>

#include "common.h"
#include "encoder.h"
#include "image.h"
#include "session.h"

namespace {

struct Request {
  std::string method, path, query, body;
  std::map<std::string, std::string> headers;
};

std::map<std::string, std::string> parseQuery(const std::string& q) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < q.size()) {
    size_t a = q.find('&', i);
    if (a == std::string::npos) a = q.size();
    std::string kv = q.substr(i, a - i);
    size_t eq = kv.find('=');
    std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
    std::string v = eq == std::string::npos ? "" : kv.substr(eq + 1);
    // Percent-decode.
    auto dec = [](const std::string& s) {
      std::string o;
      for (size_t j = 0; j < s.size(); j++) {
        if (s[j] == '%' && j + 2 < s.size()) {
          int hi = isdigit((unsigned char)s[j + 1])
                       ? s[j + 1] - '0'
                       : tolower((unsigned char)s[j + 1]) - 'a' + 10;
          int lo = isdigit((unsigned char)s[j + 2])
                       ? s[j + 2] - '0'
                       : tolower((unsigned char)s[j + 2]) - 'a' + 10;
          o += (char)(hi * 16 + lo);
          j += 2;
        } else if (s[j] == '+') {
          o += ' ';
        } else {
          o += s[j];
        }
      }
      return o;
    };
    out[dec(k)] = dec(v);
    i = a + 1;
  }
  return out;
}

std::string urlDecodePath(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = isdigit((unsigned char)s[i + 1])
                   ? s[i + 1] - '0'
                   : tolower((unsigned char)s[i + 1]) - 'a' + 10;
      int lo = isdigit((unsigned char)s[i + 2])
                   ? s[i + 2] - '0'
                   : tolower((unsigned char)s[i + 2]) - 'a' + 10;
      o += (char)(hi * 16 + lo);
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

bool sendAll(int fd, const void* data, size_t n) {
  const char* p = (const char*)data;
  while (n) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) {
      if (w < 0 && errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

std::string statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
  }
  return "OK";
}

struct Response {
  int code = 200;
  std::string contentType = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> extraHeaders;
};

Response jsonResponse(int code, const std::string& json) {
  Response r;
  r.code = code;
  r.contentType = "application/json";
  r.body = json;
  return r;
}

Response errorJson(int code, const std::string& msg) {
  J j;
  return jsonResponse(code, j.str("error", msg).done());
}

bool readRequest(int fd, Request& req) {
  std::string buf;
  char tmp[8192];
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, (size_t)n);
    if (buf.size() > 64 * 1024) return false;
    headerEnd = buf.find("\r\n\r\n");
  }
  std::string head = buf.substr(0, headerEnd);
  size_t lineEnd = head.find("\r\n");
  std::string first = head.substr(0, lineEnd);
  size_t sp1 = first.find(' ');
  size_t sp2 = first.rfind(' ');
  if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 <= sp1)
    return false;
  req.method = first.substr(0, sp1);
  std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = target.find('?');
  req.path = urlDecodePath(q == std::string::npos ? target : target.substr(0, q));
  req.query = q == std::string::npos ? "" : target.substr(q + 1);
  size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
  while (pos < head.size()) {
    size_t e = head.find("\r\n", pos);
    if (e == std::string::npos) e = head.size();
    std::string line = head.substr(pos, e - pos);
    size_t c = line.find(':');
    if (c != std::string::npos) {
      std::string k = line.substr(0, c);
      std::string v = line.substr(c + 1);
      while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
      for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
      req.headers[k] = v;
    }
    pos = e + 2;
  }
  size_t contentLen = 0;
  auto it = req.headers.find("content-length");
  if (it != req.headers.end()) contentLen = (size_t)strtoull(it->second.c_str(), nullptr, 10);
  if (contentLen > 1024 * 1024) return false;
  req.body = buf.substr(headerEnd + 4);
  while (req.body.size() < contentLen) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    req.body.append(tmp, (size_t)n);
  }
  if (req.body.size() > contentLen) req.body.resize(contentLen);
  return true;
}

std::string sessionJson(Session& s) {
  SessionSnapshot snap = s.snapshot();
  J root;
  root.str("status", snap.status);
  root.str("phase", snap.phase);
  root.str("error", snap.error);
  root.str("verificationNote", snap.verificationNote);
  root.b("sessionCreated", snap.sessionCreated);
  root.b("pipelineVerified", snap.pipelineVerified);
  root.b("hardwareCovered", snap.hardwareCovered);
  root.b("eosRequested", snap.eosRequested);
  root.b("eosComplete", snap.eosComplete);
  root.b("decoderOpen", snap.decoderOpen);
  root.b("encoderClosed", snap.encoderClosed);
  root.num("framesDrained", snap.framesDrained);
  root.num("leakedSurfaces", snap.leakedSurfaces);
  root.unum("mappedBytes", snap.mappedBytes);
  root.num("startedAtUs", snap.startedAtUs);
  root.num("endedAtUs", snap.endedAtUs);
  root.num("latestSeq", (long long)snap.latestSeq);
  root.num("version", (long long)snap.version);
  root.str("capacityHint", snap.capacityHint);
  root.str("timingSource", snap.timingSource);
  root.b("stepMode", snap.stepMode);
  root.num("pendingPackets", snap.pendingPackets);
  root.num("pendingReturns", snap.pendingReturns);
  root.num("scriptPos", (long long)snap.scriptPos);
  root.num("scriptLen", (long long)snap.scriptLen);
  root.str("trace", snap.trace);

  J cfg;
  cfg.str("backend", snap.cfg.backend);
  cfg.str("mode", snap.cfg.mode);
  cfg.num("width", snap.cfg.width).num("height", snap.cfg.height);
  cfg.num("fps", snap.cfg.fps).num("bitrate", snap.cfg.bitrate);
  cfg.num("gop", snap.cfg.gop).num("pool", snap.cfg.pool);
  cfg.num("frames", snap.cfg.frames);
  cfg.b("verify", snap.cfg.verify);
  cfg.num("receiptLagMs", snap.cfg.receiptLagMs);
  cfg.str("device", snap.cfg.device);
  cfg.str("format", snap.cfg.format);
  cfg.str("sched", snap.cfg.sched);
  cfg.num("poolMin", snap.cfg.poolMin).num("poolMax", snap.cfg.poolMax);
  cfg.num("readDelayMs", snap.cfg.readDelayMs);
  cfg.num("scanMs", snap.cfg.scanMs);
  cfg.b("step", snap.cfg.step);
  cfg.str("order", snap.cfg.order);
  cfg.str("script", snap.cfg.script);
  cfg.num("switchAt", snap.cfg.switchAt);
  cfg.num("switchWidth", snap.cfg.switchWidth);
  cfg.num("switchHeight", snap.cfg.switchHeight);
  root.raw("config", cfg.done());

  const EncoderCaps& c = snap.caps;
  J cap;
  cap.str("backendId", c.backendId);
  cap.str("displayName", c.displayName);
  cap.b("hardware", c.hardware);
  cap.b("deferredRelease", c.deferredRelease);
  cap.str("device", c.device).str("driver", c.driver).str("card", c.card);
  cap.str("bus", c.bus);
  cap.str("releaseSemantics", c.releaseSemantics);
  cap.str("receiptSemantics", c.receiptSemantics);
  cap.num("maxPool", c.maxPool);
  cap.raw("inputFormats", J::arr([&] {
            std::vector<std::string> v;
            for (auto& f : c.inputFormats) v.push_back(J::quote(f));
            return v;
          }()));
  cap.raw("notes", J::arr([&] {
            std::vector<std::string> v;
            for (auto& f : c.notes) v.push_back(J::quote(f));
            return v;
          }()));
  cap.raw("limitations", J::arr([&] {
            std::vector<std::string> v;
            for (auto& f : c.limitations) v.push_back(J::quote(f));
            return v;
          }()));
  root.raw("caps", cap.done());

  const SurfaceFormat& f = snap.format;
  J sf;
  sf.str("fmt", pixFmtName(f.fmt));
  sf.str("pixTag", f.pixTag);
  sf.num("requestedW", f.requestedW).num("requestedH", f.requestedH);
  sf.num("visibleW", f.visibleW).num("visibleH", f.visibleH);
  sf.num("codedW", f.codedW).num("codedH", f.codedH);
  sf.num("stride0", f.stride0).num("stride1", f.stride1);
  sf.num("stride2", f.stride2);
  sf.b("grantedDiffers", f.grantedDiffers);
  root.raw("format", sf.done());

  std::vector<std::string> genArr;
  for (auto& gv : snap.generations) {
    J j;
    j.num("id", gv.id);
    j.num("width", gv.width).num("height", gv.height);
    j.str("state", gv.state);
    j.num("surfaces", gv.surfaces);
    j.num("freeSurfaces", gv.freeSurfaces);
    j.unum("bytes", gv.bytes);
    genArr.push_back(j.done());
  }
  root.raw("generations", J::arr(genArr));

  std::vector<std::string> surfArr;
  for (auto& sv : snap.surfaces) {
    J j;
    j.num("index", sv.index);
    j.num("gen", sv.gen);
    j.str("state", sv.state);
    j.num("seq", sv.seq == UINT64_MAX ? -1 : (long long)sv.seq);
    j.num("reuse", sv.reuse).num("fills", sv.fills);
    j.unum("bytes", sv.bytes);
    j.b("deviceMemory", sv.deviceMemory);
    j.str("fmt", sv.fmt);
    j.num("visibleW", sv.visibleW).num("visibleH", sv.visibleH);
    j.num("codedW", sv.codedW).num("codedH", sv.codedH);
    j.num("stride0", sv.stride0).num("stride1", sv.stride1);
    j.num("stride2", sv.stride2);
    j.str("releaseCond", sv.releaseCond);
    surfArr.push_back(j.done());
  }
  root.raw("surfaces", J::arr(surfArr));

  std::vector<std::string> frameArr;
  for (auto& fv : snap.frames) {
    J j;
    j.num("seq", fv.seq);
    j.num("order", fv.order);
    j.num("surfaceIndex", fv.surfaceIndex);
    j.num("gen", fv.gen);
    j.str("status", fv.status);
    j.b("bitstreamReady", fv.bitstreamReady);
    j.b("surfaceReusable", fv.surfaceReusable);
    j.num("markerExpected", fv.markerExpected);
    j.num("markerDecoded", fv.markerDecoded);
    j.b("markerOk", fv.markerOk);
    j.b("patchesOk", fv.patchesOk);
    j.b("colorsOk", fv.colorsOk);
    j.b("decodeClean", fv.decodeClean);
    j.b("corrupted", fv.corrupted);
    j.b("keyframe", fv.keyframe);
    j.num("cornerExpected", fv.cornerExpected);
    j.raw("cornerTags", J::arr({std::to_string(fv.cornerTags[0]),
                                std::to_string(fv.cornerTags[1]),
                                std::to_string(fv.cornerTags[2]),
                                std::to_string(fv.cornerTags[3])}));
    j.num("cornerValid", fv.cornerValid);
    j.b("cornersOk", fv.cornersOk);
    j.b("cornersMixed", fv.cornersMixed);
    j.num("auBytes", fv.auBytes);
    j.dbl("psnrMae", fv.meanAbsError);
    j.dbl("submitToAuMs", fv.submitToAuMs);
    j.dbl("submitToReleaseMs", fv.submitToReleaseMs);
    j.num("decodedW", fv.decodedW).num("decodedH", fv.decodedH);
    j.b("hasSrc", fv.hasSrc);
    j.b("hasOut", fv.hasOut);
    j.unum("auCrc", fv.auCrc);
    j.str("note", fv.note);
    frameArr.push_back(j.done());
  }
  root.raw("frames", J::arr(frameArr));

  const RunMetrics& m = snap.metrics;
  J mj;
  mj.num("submitted", m.submitted).num("accepted", m.accepted);
  mj.num("bitstreams", m.bitstreams).num("released", m.released);
  mj.num("verified", m.verified).num("failed", m.failed);
  mj.num("detected", m.detected);
  mj.num("skipped", m.skipped).num("lost", m.lost);
  mj.num("outOfOrder", m.outOfOrder).num("duplicateSeq", m.duplicateSeq);
  mj.num("orphanAu", m.orphanAu);
  mj.num("decodeErrors", m.decodeErrors);
  mj.num("markerMismatches", m.markerMismatches);
  mj.num("patchMismatches", m.patchMismatches);
  mj.num("colorMismatches", m.colorMismatches);
  mj.num("pictureMismatches", m.pictureMismatches);
  mj.num("backpressureStalls", m.backpressureStalls);
  mj.dbl("stallMsTotal", m.stallMsTotal);
  mj.num("maxInFlight", m.maxInFlight);
  mj.num("guardChecks", m.guardChecks);
  mj.num("guardRejections", m.guardRejections);
  mj.num("cornerMismatches", m.cornerMismatches);
  mj.num("mixedFrames", m.mixedFrames);
  mj.num("ownershipViolations", m.ownershipViolations);
  mj.num("staleGenReleases", m.staleGenReleases);
  mj.num("grows", m.grows).num("shrinks", m.shrinks);
  mj.num("schedWaits", m.schedWaits);
  mj.num("peakActiveSurfaces", m.peakActiveSurfaces);
  mj.unum("peakMappedBytes", m.peakMappedBytes);
  mj.num("peakRssKb", m.peakRssKb).num("baselineRssKb", m.baselineRssKb);
  mj.unum("surfaceBytes", m.surfaceBytes);
  mj.num("framesDrained", m.framesDrained);
  mj.dbl("p50SubmitToAuMs", m.p50SubmitToAuMs);
  mj.dbl("maxSubmitToAuMs", m.maxSubmitToAuMs);
  mj.dbl("p50SubmitToReleaseMs", m.p50SubmitToReleaseMs);
  mj.dbl("maxSubmitToReleaseMs", m.maxSubmitToReleaseMs);
  root.raw("metrics", mj.done());

  std::vector<std::string> logArr;
  for (auto& l : snap.log) logArr.push_back(J::quote(l));
  root.raw("log", J::arr(logArr));
  return root.done();
}

std::string devicesJson() {
  std::vector<std::string> arr;
  auto probes = probeV4l2Devices(0, 0);
  for (auto& p : probes) {
    J j;
    j.str("path", p.path);
    j.str("driver", p.driver).str("card", p.card).str("bus", p.bus);
    j.b("m2m", p.m2m).b("h264Capture", p.h264Capture);
    j.str("note", p.note);
    j.raw("outputFormats", J::arr([&] {
            std::vector<std::string> v;
            for (auto& f : p.outputFormats) v.push_back(J::quote(f));
            return v;
          }()));
    arr.push_back(j.done());
  }
  EncoderCaps sw = softwareCaps();
  J root;
  root.b("v4l2Available", !probes.empty());
  if (probes.empty()) {
    root.str("v4l2Reason",
             "未发现带 V4L2_CAP_VIDEO_M2M(_MPLANE) 的 /dev/video* 节点");
  }
  root.raw("devices", J::arr(arr));
  J s;
  s.str("id", "openh264");
  s.str("name", sw.displayName);
  s.b("hardware", false);
  s.str("note",
        "软件 H.264 对照与接口模型，任何主机均可运行；不是硬件测量。");
  root.raw("software", s.done());
  return root.done();
}

Response route(Session& session, const Request& req) {
  const std::string& p = req.path;
  if (req.method == "GET" && (p == "/" || p == "/index.html")) {
    return Response();  // handled by static file path below
  }
  if (p == "/api/state") {
    if (req.method != "GET") return errorJson(405, "GET only");
    return jsonResponse(200, sessionJson(session));
  }
  if (p == "/api/devices") return jsonResponse(200, devicesJson());
  if (p == "/api/start") {
    if (req.method != "POST") return errorJson(405, "POST only");
    auto q = parseQuery(req.query);
    auto getS = [&](const char* k, const std::string& def) {
      auto it = q.find(k);
      return it == q.end() || it->second.empty() ? def : it->second;
    };
    auto getI = [&](const char* k, int def) {
      auto it = q.find(k);
      return it == q.end() || it->second.empty() ? def : atoi(it->second.c_str());
    };
    SessionConfig c;
    c.backend = getS("backend", "openh264");
    c.mode = getS("mode", "paced");
    c.width = getI("width", 640);
    c.height = getI("height", 360);
    c.fps = getI("fps", 30);
    c.bitrate = getI("bitrate", 3000000);
    c.gop = getI("gop", 30);
    c.pool = getI("pool", 4);
    c.frames = getI("frames", 60);
    c.verify = getI("verify", 1) != 0;
    c.receiptLagMs = getI("receiptLagMs", 0);
    c.device = getS("device", "");
    c.format = getS("format", "");
    c.sched = getS("sched", "fixed");
    c.poolMin = getI("poolMin", 2);
    c.poolMax = getI("poolMax", 8);
    c.readDelayMs = getI("readDelayMs", 0);
    c.scanMs = getI("scanMs", 0);
    c.step = getI("step", 0) != 0;
    c.order = getS("order", "any");
    c.script = getS("script", "");
    c.switchAt = getI("switchAt", -1);
    c.switchWidth = getI("switchW", 0);
    c.switchHeight = getI("switchH", 0);
    std::string err;
    if (!session.start(c, err)) return errorJson(409, err);
    J j;
    return jsonResponse(200, j.b("started", true).done());
  }
  if (p == "/api/step") {
    if (req.method != "POST") return errorJson(405, "POST only");
    auto q = parseQuery(req.query);
    std::string kind = q.count("kind") ? q["kind"] : "any";
    int n = q.count("n") ? atoi(q["n"].c_str()) : 1;
    std::string err;
    if (!session.stepEvents(kind, n, err)) return errorJson(409, err);
    J j;
    return jsonResponse(200, j.b("stepped", true).done());
  }
  if (p == "/api/continue") {
    if (req.method != "POST") return errorJson(405, "POST only");
    session.continueRun();
    J j;
    return jsonResponse(200, j.b("continued", true).done());
  }
  if (p == "/api/stop") {
    if (req.method != "POST") return errorJson(405, "POST only");
    session.stop();
    J j;
    return jsonResponse(200, j.b("stopping", true).done());
  }
  if (p == "/api/frame.png") {
    auto q = parseQuery(req.query);
    std::string kind = q.count("kind") ? q["kind"] : "out";
    uint64_t seq = q.count("seq") && !q["seq"].empty()
                       ? strtoull(q["seq"].c_str(), nullptr, 10)
                       : UINT64_MAX;
    int maxW = q.count("w") ? atoi(q["w"].c_str()) : 360;
    std::vector<uint8_t> png;
    if (!session.framePng(kind, seq, maxW, png))
      return errorJson(404, "no picture available for that selection");
    Response r;
    r.code = 200;
    r.contentType = "image/png";
    r.body.assign((const char*)png.data(), png.size());
    r.extraHeaders.push_back({"Cache-Control", "no-store"});
    return r;
  }
  if (p == "/api/au") {
    auto q = parseQuery(req.query);
    uint64_t seq = q.count("seq") ? strtoull(q["seq"].c_str(), nullptr, 10)
                                  : UINT64_MAX;
    std::vector<uint8_t> data;
    bool key = false;
    if (!session.accessUnit(seq, data, key))
      return errorJson(404, "no access unit retained for that frame");
    Response r;
    r.code = 200;
    r.contentType = "application/octet-stream";
    r.body.assign((const char*)data.data(), data.size());
    r.extraHeaders.push_back({"X-Keyframe", key ? "1" : "0"});
    r.extraHeaders.push_back({"X-Frame-Seq", std::to_string(seq)});
    r.extraHeaders.push_back({"Cache-Control", "no-store"});
    return r;
  }
  return errorJson(404, "no such endpoint");
}

bool serveStatic(const std::string& root, const std::string& path,
                 Response& out) {
  std::string rel = path == "/" ? "/index.html" : path;
  if (rel.find("..") != std::string::npos) return false;
  std::string full = root + rel;
  struct stat st;
  if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  if (st.st_size > 4 * 1024 * 1024) return false;
  int fd = open(full.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out.body.resize((size_t)st.st_size);
  size_t got = 0;
  while (got < out.body.size()) {
    ssize_t n = read(fd, &out.body[got], out.body.size() - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  out.body.resize(got);
  std::string ext;
  size_t dot = rel.rfind('.');
  if (dot != std::string::npos) ext = rel.substr(dot);
  if (ext == ".html") out.contentType = "text/html; charset=utf-8";
  else if (ext == ".js") out.contentType = "text/javascript; charset=utf-8";
  else if (ext == ".css") out.contentType = "text/css; charset=utf-8";
  else if (ext == ".png") out.contentType = "image/png";
  else if (ext == ".svg") out.contentType = "image/svg+xml";
  else if (ext == ".otf") out.contentType = "font/otf";
  else if (ext == ".woff2") out.contentType = "font/woff2";
  else out.contentType = "application/octet-stream";
  out.code = 200;
  out.extraHeaders.push_back({"Cache-Control", "no-store"});
  return true;
}

void handleConnection(int fd, Session& session, const std::string& webRoot) {
  struct timeval tv = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  Request req;
  if (!readRequest(fd, req)) {
    close(fd);
    return;
  }
  Response res;
  if (req.path.rfind("/api/", 0) == 0) {
    res = route(session, req);
  } else {
    bool ok = serveStatic(webRoot, req.path, res);
    if (!ok) res = errorJson(404, "not found");
  }
  std::string head = "HTTP/1.1 " + std::to_string(res.code) + " " +
                     statusText(res.code) + "\r\n";
  head += "Content-Type: " + res.contentType + "\r\n";
  head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
  head += "Connection: close\r\n";
  for (auto& h : res.extraHeaders) head += h.first + ": " + h.second + "\r\n";
  head += "\r\n";
  sendAll(fd, head.data(), head.size());
  if (req.method != "HEAD") sendAll(fd, res.body.data(), res.body.size());
  close(fd);
}

}  // namespace

bool runServer(Session& session, const ServerConfig& cfg, std::string& err,
               const std::atomic<bool>* stopFlag) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = std::string("socket: ") + strerror(errno);
    return false;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)cfg.port);
  if (inet_pton(AF_INET, cfg.host.c_str(), &addr.sin_addr) != 1) {
    err = "invalid host";
    close(fd);
    return false;
  }
  if (bind(fd, (sockaddr*)&addr, sizeof addr) != 0) {
    err = std::string("bind ") + cfg.host + ":" + std::to_string(cfg.port) +
          ": " + strerror(errno);
    close(fd);
    return false;
  }
  if (listen(fd, 32) != 0) {
    err = std::string("listen: ") + strerror(errno);
    close(fd);
    return false;
  }
  printf("hardware/software encoder lab listening on http://%s:%d/\n",
         cfg.host.c_str(), cfg.port);
  printf("open that URL; API is under /api/. Ctrl+C to stop.\n");
  fflush(stdout);
  for (;;) {
    if (stopFlag && stopFlag->load()) break;
    pollfd pfd{fd, POLLIN, 0};
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      err = std::string("poll: ") + strerror(errno);
      close(fd);
      return false;
    }
    if (pr == 0) continue;
    int c = accept(fd, nullptr, nullptr);
    if (c < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
      err = std::string("accept: ") + strerror(errno);
      close(fd);
      return false;
    }
    std::thread(handleConnection, c, std::ref(session), cfg.webRoot).detach();
  }
  close(fd);
  return true;
}
