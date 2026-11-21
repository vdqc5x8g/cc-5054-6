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

struct SelftestOpts {
  std::string backend;
  std::string mode = "paced";
  std::string sched = "fixed";
  int pool = 3;
  int poolMin = 2;
  int poolMax = 8;
  std::string scenario;  // "" | switch | replay | ab
  std::string order = "any";
  std::string script;
  int readDelayMs = 0;
  int scanMs = 0;
};

SessionConfig baseConfig(const SelftestOpts& o) {
  SessionConfig cfg;
  cfg.backend = o.backend;
  cfg.mode = o.mode;
  cfg.width = 640;
  cfg.height = 360;
  cfg.frames = 30;
  cfg.pool = o.pool;
  cfg.gop = 10;
  cfg.sched = o.sched;
  cfg.poolMin = o.poolMin;
  cfg.poolMax = o.poolMax;
  cfg.order = o.order;
  cfg.script = o.script;
  cfg.readDelayMs = o.readDelayMs;
  cfg.scanMs = o.scanMs;
  if (o.scenario == "switch") {
    cfg.switchAt = cfg.frames / 2;
    cfg.switchWidth = 480;
    cfg.switchHeight = 270;
  }
  return cfg;
}

// Runs one configuration to completion and returns the final snapshot.
SessionSnapshot runOnce(const SessionConfig& cfg) {
  Session session;
  std::string err;
  if (!session.start(cfg, err)) {
    fprintf(stderr, "自检启动失败：%s\n", err.c_str());
    SessionSnapshot s;
    s.status = "failed";
    s.error = err;
    return s;
  }
  int waited = 0;
  const int limitMs = 60000;
  while (waited < limitMs) {
    SessionSnapshot s = session.snapshot();
    if (s.status == "completed" || s.status == "failed" ||
        s.status == "stopped")
      break;
    sleepUs(20000);
    waited += 20;
  }
  session.join(8000);
  return session.snapshot();
}

void printReport(const SessionSnapshot& s) {
  const RunMetrics& m = s.metrics;
  printf("后端            : %s\n", s.cfg.backend.c_str());
  printf("送帧方式        : %s\n", s.cfg.mode.c_str());
  printf("表面调度器      : %s（池 %d，下限 %d，上限 %d）\n", s.cfg.sched.c_str(),
         s.cfg.pool, s.cfg.poolMin, s.cfg.poolMax);
  printf("会话创建        : %s\n", s.sessionCreated ? "成功" : "失败");
  printf("硬件覆盖        : %s\n",
         s.hardwareCovered ? "真实设备" : "未覆盖（本机软件对照）");
  printf("时序来源        : %s\n",
         s.timingSource == "device" ? "真实设备" : "软件模型模拟");
  printf("输出格式        : %s %dx%d 步幅 %d/%d\n", s.format.pixTag.c_str(),
         s.format.codedW, s.format.codedH, s.format.stride0,
         s.format.stride1);
  printf("表面池          : %zu 个 / %zu 字节\n", s.surfaces.size(),
         s.mappedBytes);
  if (!s.generations.empty()) {
    printf("代次            : ");
    for (auto& g : s.generations)
      printf("[#%d %dx%d %s %d 个表面] ", g.id, g.width, g.height,
             g.state.c_str(), g.surfaces);
    printf("\n");
  }
  printf("提交 / 码流     : %d / %d\n", m.submitted, m.bitstreams);
  printf("核对通过/失败/检出: %d / %d / %d\n", m.verified, m.failed,
         m.detected);
  printf("乱序 / 重复     : %d / %d\n", m.outOfOrder, m.duplicateSeq);
  printf("背压等待        : %d 次，累计 %.1f ms\n", m.backpressureStalls,
         m.stallMsTotal);
  printf("最大在途表面    : %d\n", m.maxInFlight);
  printf("弹性增长/收缩/等待: %d / %d / %d\n", m.grows, m.shrinks,
         m.schedWaits);
  printf("峰值活跃表面    : %d\n", m.peakActiveSurfaces);
  printf("峰值映射字节    : %zu\n", m.peakMappedBytes);
  printf("串帧 / 错误角标 : %d / %d\n", m.mixedFrames, m.cornerMismatches);
  printf("越权写入        : %d\n", m.ownershipViolations);
  printf("跨代次迟到归还  : %d\n", m.staleGenReleases);
  printf("守卫检查/拒绝   : %d / %d\n", m.guardChecks, m.guardRejections);
  printf("未归还表面      : %d\n", s.leakedSurfaces);
  printf("EOS 排空        : %s\n", s.eosComplete ? "完成" : "未完成");
  printf("RSS 基线/峰值   : %d / %d KB\n", m.baselineRssKb, m.peakRssKb);
  printf("提交→码流 p50/最大: %.1f / %.1f ms\n", m.p50SubmitToAuMs,
         m.maxSubmitToAuMs);
  printf("提交→可复用 p50/最大: %.1f / %.1f ms\n", m.p50SubmitToReleaseMs,
         m.maxSubmitToReleaseMs);
  if (!s.trace.empty())
    printf("调度轨迹        : %s%s\n", s.trace.substr(0, 96).c_str(),
           s.trace.size() > 96 ? "…" : "");
  printf("容量建议        : %s\n", s.capacityHint.c_str());
  printf("判定            : %s\n", s.verificationNote.c_str());
}

bool runPasses(const SessionSnapshot& s, const SelftestOpts& o) {
  if (!s.pipelineVerified) return false;
  const RunMetrics& m = s.metrics;
  if (o.mode == "earlyreuse") {
    // The injected premature reuse must have happened exactly once and the
    // torn frame must have been identified via its corner tags.
    if (m.ownershipViolations != 1 || m.mixedFrames != 1 || m.detected != 1)
      return false;
  }
  if (o.scenario == "switch" && m.staleGenReleases < 1) return false;
  return true;
}

int runSelfTest(const SelftestOpts& o) {
  if (o.scenario == "replay") {
    // Run A records a schedule; run B replays it verbatim. Both must verify
    // and produce the identical event trace.
    SelftestOpts a = o;
    a.order = "packet";
    a.script.clear();
    SessionSnapshot sa = runOnce(baseConfig(a));
    printf("== 第一次（记录轨迹，产包优先）==\n");
    printReport(sa);
    SelftestOpts b = o;
    b.order = "any";
    b.script = sa.trace;
    SessionSnapshot sb = runOnce(baseConfig(b));
    printf("== 第二次（按轨迹重放）==\n");
    printReport(sb);
    bool pass = sa.pipelineVerified && sb.pipelineVerified &&
                !sa.trace.empty() && sa.trace == sb.trace;
    printf("轨迹一致        : %s（%zu 个事件）\n",
           sa.trace == sb.trace ? "是" : "否", sa.trace.size());
    printf("RESULT          : %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  if (o.scenario == "ab") {
    // Same load, two event-order trajectories: packet-first vs return-first.
    SelftestOpts a = o;
    a.order = "packet";
    SessionSnapshot sa = runOnce(baseConfig(a));
    printf("== 轨迹 A：产包事件优先 ==\n");
    printReport(sa);
    SelftestOpts b = o;
    b.order = "return";
    SessionSnapshot sb = runOnce(baseConfig(b));
    printf("== 轨迹 B：归还事件优先 ==\n");
    printReport(sb);
    printf("== 对照（A=产包优先，B=归还优先）==\n");
    printf("内存峰值 RSS KB : A %d / B %d\n", sa.metrics.peakRssKb,
           sb.metrics.peakRssKb);
    printf("峰值映射字节    : A %zu / B %zu\n", sa.metrics.peakMappedBytes,
           sb.metrics.peakMappedBytes);
    printf("阻塞次数/毫秒   : A %d/%.1f / B %d/%.1f\n",
           sa.metrics.backpressureStalls, sa.metrics.stallMsTotal,
           sb.metrics.backpressureStalls, sb.metrics.stallMsTotal);
    printf("错误角标/串帧   : A %d/%d / B %d/%d\n",
           sa.metrics.cornerMismatches, sa.metrics.mixedFrames,
           sb.metrics.cornerMismatches, sb.metrics.mixedFrames);
    bool pass = sa.pipelineVerified && sb.pipelineVerified;
    printf("RESULT          : %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  SessionSnapshot s = runOnce(baseConfig(o));
  printReport(s);
  bool pass = runPasses(s, o);
  printf("RESULT          : %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
  ServerConfig cfg;
  SelftestOpts st;
  bool selfTest = false;
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
      st.backend = needsValue("--selftest");
    } else if (a == "--mode") st.mode = needsValue("--mode");
    else if (a == "--sched") st.sched = needsValue("--sched");
    else if (a == "--pool") st.pool = atoi(needsValue("--pool").c_str());
    else if (a == "--poolmin") st.poolMin = atoi(needsValue("--poolmin").c_str());
    else if (a == "--poolmax") st.poolMax = atoi(needsValue("--poolmax").c_str());
    else if (a == "--scenario") st.scenario = needsValue("--scenario");
    else if (a == "--order") st.order = needsValue("--order");
    else if (a == "--script") st.script = needsValue("--script");
    else if (a == "--readdelay") st.readDelayMs = atoi(needsValue("--readdelay").c_str());
    else if (a == "--scan") st.scanMs = atoi(needsValue("--scan").c_str());
    else if (a == "--help" || a == "-h") {
      printf(
          "用法：encoder_lab [--host 127.0.0.1] [--port 8099] [--web web]\n"
          "      encoder_lab --selftest <openh264|v4l2> [--mode <mode>]\n"
          "          [--sched fixed|elastic] [--pool N] [--poolmin N] [--poolmax N]\n"
          "          [--scenario switch|replay|ab] [--order any|packet|return]\n"
          "          [--script prpr...] [--readdelay ms] [--scan ms]\n\n"
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
    if (st.backend != "openh264" && st.backend != "v4l2") {
      fprintf(stderr, "--selftest 取值必须为 openh264 或 v4l2\n");
      return 2;
    }
    if (!st.scenario.empty() && st.scenario != "switch" &&
        st.scenario != "replay" && st.scenario != "ab") {
      fprintf(stderr, "--scenario 取值必须为 switch、replay 或 ab\n");
      return 2;
    }
    return runSelfTest(st);
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
