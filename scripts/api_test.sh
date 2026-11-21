#!/usr/bin/env bash
# 启动真实服务，通过 HTTP API 跑一次完整运行并校验结果。
set -u
cd "$(dirname "$0")/.."

BIN=build/encoder_lab
PORT=${PORT:-8099}
BASE="http://127.0.0.1:$PORT"
make -s || { echo "构建失败"; exit 1; }

"$BIN" --port "$PORT" >/tmp/encoder_lab_api_test.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null' EXIT

# 等端口就绪
for _ in $(seq 1 100); do
  curl -sf "$BASE/api/state" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "$BASE/api/state" >/dev/null || { echo "服务未就绪"; cat /tmp/encoder_lab_api_test.log; exit 1; }

fail=0
check() { if [ "$2" = "$3" ]; then echo "ok   $1 = $3"; else echo "FAIL $1 = $2（期望 $3）"; fail=1; fi; }

echo "== 设备探测"
DEV=$(curl -sf "$BASE/api/devices")
python3 -I -c '
import json,sys
d=json.load(sys.stdin)
assert d["software"]["id"]=="openh264", d
print("ok   软件对照后端: "+d["software"]["name"])
print("ok   硬件节点数: %d" % len(d["devices"]))
' <<<"$DEV" || { echo "FAIL /api/devices"; fail=1; }

echo "== 启动一次 burst 运行"
curl -sf -X POST "$BASE/api/start?backend=openh264&mode=burst&frames=24&pool=3&width=320&height=180&gop=8" >/dev/null \
  || { echo "FAIL /api/start"; exit 1; }

for _ in $(seq 1 300); do
  ST=$(curl -sf "$BASE/api/state")
  STATUS=$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["status"])' <<<"$ST")
  case "$STATUS" in completed|failed|stopped) break;; esac
  sleep 0.1
done

python3 -I - "$BASE" "$ST" <<'PY'
import json,sys,urllib.request
base=sys.argv[1]; s=json.loads(sys.argv[2])
m=s["metrics"]
ok=True
def check(name,got,want):
    global ok
    if got==want: print("ok   %s = %s"%(name,got))
    else: print("FAIL %s = %s（期望 %s）"%(name,got,want)); ok=False

check("status", s["status"], "completed")
check("sessionCreated", s["sessionCreated"], True)
check("软件对照未冒充硬件", s["caps"]["hardware"], False)
check("pipelineVerified", s["pipelineVerified"], True)
check("submitted", m["submitted"], s["config"]["frames"])
check("bitstreams", m["bitstreams"], s["config"]["frames"])
check("verified", m["verified"], s["config"]["frames"])
check("failed", m["failed"], 0)
check("detected", m["detected"], 0)
check("leakedSurfaces", s["leakedSurfaces"], 0)
check("eosComplete", s["eosComplete"], True)
check("hardwareCovered", s["hardwareCovered"], False)
check("surface 数量=池大小", len(s["surfaces"]), s["config"]["pool"])

seqs=[f["seq"] for f in s["frames"]]
check("帧号连续", seqs, list(range(len(seqs))))
check("每帧有码流", all(f["bitstreamReady"] for f in s["frames"]), True)
check("每帧可复用", all(f["surfaceReusable"] for f in s["frames"]), True)
check("解码出可见画面", all(f["decodedW"]==s["format"]["visibleW"] and f["decodedH"]==s["format"]["visibleH"] for f in s["frames"]), True)

# 表面实际协商布局必须真实：步幅不为 0，且不小于可见宽度
fmt=s["format"]
check("可见宽度", fmt["visibleW"], 320)
check("可见高度", fmt["visibleH"], 180)
check("Y 步幅≥宽度", fmt["stride0"]>=fmt["visibleW"], True)
check("编码宽度按对齐扩展", fmt["codedW"]%16, 0)
check("编码高度按对齐扩展", fmt["codedH"]%16, 0)

# 真实取图与码流
png=urllib.request.urlopen(base+"/api/frame.png?kind=out&seq=5").read()
check("解码输出 PNG 头", png[:4], b"\x89PNG")
src=urllib.request.urlopen(base+"/api/frame.png?kind=src&seq=5").read()
check("源画面 PNG 头", src[:4], b"\x89PNG")
r=urllib.request.urlopen(base+"/api/au?seq=5")
au=r.read()
check("码流大小 > 0", len(au)>0, True)
check("码流 seq 头", r.headers.get("X-Frame-Seq"), "5")

sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -ne 0 ] && fail=1

echo "== 单步放行：事件门逐事件放行并记录轨迹"
curl -sf -X POST "$BASE/api/start?backend=openh264&mode=late&frames=10&pool=3&receiptLagMs=150&step=1" >/dev/null \
  || { echo "FAIL /api/start step=1"; exit 1; }
GATE=""
for _ in $(seq 1 200); do
  ST=$(curl -sf "$BASE/api/state")
  GATE=$(python3 -I -c 'import json,sys;s=json.load(sys.stdin);print(s["pendingPackets"]+s["pendingReturns"])' <<<"$ST")
  [ "$GATE" -gt 0 ] 2>/dev/null && break
  sleep 0.1
done
check "事件门拦住待放行事件" "$([ "$GATE" -gt 0 ] && echo yes)" "yes"
check "单步模式标记" "$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["stepMode"])' <<<"$ST")" "True"
check "未放行前轨迹为空" "$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["trace"])' <<<"$ST")" ""
for i in 1 2 3 4; do
  curl -sf -X POST "$BASE/api/step?kind=any&n=1" >/dev/null || { echo "FAIL /api/step"; fail=1; }
  sleep 0.1
done
ST=$(curl -sf "$BASE/api/state")
check "步进 4 次后轨迹长度" "$(python3 -I -c 'import json,sys;print(len(json.load(sys.stdin)["trace"]))' <<<"$ST")" "4"
curl -sf -X POST "$BASE/api/continue" >/dev/null || { echo "FAIL /api/continue"; fail=1; }
for _ in $(seq 1 300); do
  ST=$(curl -sf "$BASE/api/state")
  STATUS=$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["status"])' <<<"$ST")
  case "$STATUS" in completed|failed|stopped) break;; esac
  sleep 0.1
done
TRACE=$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["trace"])' <<<"$ST")
check "单步运行完成" "$STATUS" "completed"
check "轨迹非空" "$([ -n "$TRACE" ] && echo yes)" "yes"
check "时序来源标记为模拟" "$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["timingSource"])' <<<"$ST")" "simulated"
check "容量建议非空" "$(python3 -I -c 'import json,sys;print(bool(json.load(sys.stdin)["capacityHint"]))' <<<"$ST")" "True"

echo "== 轨迹重放：同一脚本必须复现同一轨迹"
curl -sf -X POST "$BASE/api/start?backend=openh264&mode=late&frames=10&pool=3&receiptLagMs=150&script=$TRACE" >/dev/null \
  || { echo "FAIL /api/start script"; exit 1; }
for _ in $(seq 1 300); do
  ST=$(curl -sf "$BASE/api/state")
  STATUS=$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["status"])' <<<"$ST")
  case "$STATUS" in completed|failed|stopped) break;; esac
  sleep 0.1
done
check "重放运行完成" "$STATUS" "completed"
check "重放轨迹一致" "$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["trace"])' <<<"$ST")" "$TRACE"
check "重放链路验收" "$(python3 -I -c 'import json,sys;print(json.load(sys.stdin)["pipelineVerified"])' <<<"$ST")" "True"

echo "== 404 行为"
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/api/au?seq=99999")
check "未知帧码流返回 404" "$code" "404"
code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/../etc/passwd")
check "路径穿越被拒绝" "$code" "404"

if [ $fail -ne 0 ]; then echo "API 测试：失败"; exit 1; fi
echo "API 测试：全部通过"
