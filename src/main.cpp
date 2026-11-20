#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "session.h"
#include "server.h"

namespace {
Session* g_session = nullptr;
std::atomic<bool> g_stop{false};

void onSignal(int) {
  g_stop.store(true);
  if (g_session) g_session->stop();
}

int runSelfTest(const std::string& backend, const std::string& mode) {
  Session session;
  SessionConfig cfg;
  cfg.backend = backend;
  cfg.mode = mode;
  cfg.width = 640;
  cfg.height = 360;
  cfg.frames = 30;
  cfg.pool = 3;
  cfg.gop = 10;
  std::string err;
  if (!session.start(cfg, err)) {
    fprintf(stderr, "自检启动失败：%s\n", err.c_str());
    return 2;
  }
  int waited = 0;
  const int limitMs = 30000;
  while (waited < limitMs) {
    SessionSnapshot s = session.snapshot();
    if (s.status == "completed" || s.status == "failed" || s.status == "stopped")
      break;
    sleepUs(20000);
    waited += 20;
  }
  session.join(5000);
  SessionSnapshot s = session.snapshot();
  const RunMetrics& m = s.metrics;
  printf("后端            : %s\n", s.cfg.backend.c_str());
  printf("送帧方式        : %s\n", s.cfg.mode.c_str());
  printf("会话创建        : %s\n", s.sessionCreated ? "成功" : "失败");
  printf("硬件覆盖        : %s\n", s.hardwareCovered ? "真实设备" : "未覆盖（本机软件对照）");
  printf("输出格式        : %s %dx%d 步幅 %d/%d\n", s.format.pixTag.c_str(),
         s.format.codedW, s.format.codedH, s.format.stride0, s.format.stride1);
  printf("表面池          : %zu 个 / %zu 字节\n", s.surfaces.size(),
         s.mappedBytes);
  printf("提交 / 码流     : %d / %d\n", m.submitted, m.bitstreams);
  printf("核对通过/失败/检出: %d / %d / %d\n", m.verified, m.failed, m.detected);
  printf("乱序 / 重复     : %d / %d\n", m.outOfOrder, m.duplicateSeq);
  printf("背压等待        : %d 次，累计 %.1f ms\n", m.backpressureStalls,
         m.stallMsTotal);
  printf("最大在途表面    : %d\n", m.maxInFlight);
  printf("守卫检查/拒绝   : %d / %d\n", m.guardChecks, m.guardRejections);
  printf("未归还表面      : %d\n", s.leakedSurfaces);
  printf("EOS 排空        : %s\n", s.eosComplete ? "完成" : "未完成");
  printf("RSS 基线/峰值   : %d / %d KB\n", m.baselineRssKb, m.peakRssKb);
  printf("提交→码流 p50/最大: %.1f / %.1f ms\n", m.p50SubmitToAuMs,
         m.maxSubmitToAuMs);
  printf("提交→可复用 p50/最大: %.1f / %.1f ms\n", m.p50SubmitToReleaseMs,
         m.maxSubmitToReleaseMs);
  printf("判定            : %s\n", s.verificationNote.c_str());
  bool pass = s.pipelineVerified;
  printf("RESULT          : %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  ServerConfig cfg;
  std::string selfTestBackend;
  bool selfTest = false;
  std::string selfTestMode = "paced";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto needsValue = [&](const char* name) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s 需要一个取值\n", name);
        exit(2);
      }
      return std::string(argv[++i]);
    };
    if (a == "--port") cfg.port = atoi(needsValue("--port").c_str());
    else if (a == "--host") cfg.host = needsValue("--host");
    else if (a == "--web") cfg.webRoot = needsValue("--web");
    else if (a == "--selftest") {
      selfTest = true;
      selfTestBackend = needsValue("--selftest");
    } else if (a == "--mode") selfTestMode = needsValue("--mode");
    else if (a == "--help" || a == "-h") {
      printf(
          "用法：encoder_lab [--host 127.0.0.1] [--port 8099] [--web web]\n"
          "      encoder_lab --selftest <openh264|v4l2> [--mode <mode>]\n\n"
          "默认启动硬件编码接入实验台界面与 JSON API。\n"
          "--selftest 在无浏览器环境下同步运行一次并打印报告（PASS/FAIL）。\n");
      return 0;
    } else {
      fprintf(stderr, "未知参数：%s\n", a.c_str());
      return 2;
    }
  }

  signal(SIGPIPE, SIG_IGN);
  if (selfTest) {
    if (selfTestBackend != "openh264" && selfTestBackend != "v4l2") {
      fprintf(stderr, "--selftest 取值必须为 openh264 或 v4l2\n");
      return 2;
    }
    return runSelfTest(selfTestBackend, selfTestMode);
  }

  Session session;
  g_session = &session;
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  std::string err;
  if (!runServer(session, cfg, err, &g_stop)) {
    fprintf(stderr, "服务启动失败：%s\n", err.c_str());
    return 1;
  }
  session.stop();
  return 0;
}
