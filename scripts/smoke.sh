#!/usr/bin/env bash
# 构建并跑完五种送帧方式的自检。每个方式都必须 PASS。
set -u
cd "$(dirname "$0")/.."

BIN=build/encoder_lab
make -s || { echo "构建失败"; exit 1; }

fail=0
run() {
  local mode=$1 expect=${2:-}
  echo "================================================================"
  echo "== 送帧方式：$mode"
  echo "================================================================"
  local out
  out=$("$BIN" --selftest openh264 --mode "$mode" 2>&1)
  local rc=$?
  printf '%s\n' "$out"
  if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
    echo "!! $mode 未通过"
    fail=1
  fi
  # 反例检查：软件结果不能冒充硬件覆盖
  if grep -q '硬件覆盖 *: 真实设备' <<<"$out"; then
    echo "!! $mode 把软件对照标成了真实设备"
    fail=1
  fi
  # 有注入/守卫语义的方式要真的触发对应计数
  case "$expect" in
    guard)
      if ! grep -Eq '守卫检查/拒绝 *: [0-9]+ / [1-9][0-9]*$' <<<"$out"; then
        echo "!! guard 没有产生任何拒绝"; fail=1
      fi
      ;;
    corrupt)
      if ! grep -q '核对通过/失败/检出: .* / .* / 1$' <<<"$out"; then
        echo "!! corrupt 没有恰好检出 1 帧"; fail=1
      fi
      ;;
  esac
}

run paced
run burst
run late
run guard guard
run corrupt corrupt

echo "================================================================"
echo "== 提前复用（earlyreuse）：串帧必须被角标识破"
echo "================================================================"
out=$("$BIN" --selftest openh264 --mode earlyreuse 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! earlyreuse 未通过"; fail=1
fi
grep -q '串帧 / 错误角标 *: 1 / 0' <<<"$out" || { echo "!! earlyreuse 没有恰好 1 帧串帧"; fail=1; }
grep -q '越权写入 *: 1' <<<"$out" || { echo "!! earlyreuse 没有恰好 1 次越权写入"; fail=1; }

echo "================================================================"
echo "== 弹性调度：late 回执下应增长消除阻塞"
echo "================================================================"
out=$("$BIN" --selftest openh264 --mode late --sched elastic --poolmin 2 --poolmax 6 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! elastic(late) 未通过"; fail=1
fi
grep -Eq '弹性增长/收缩/等待: [1-9][0-9]* / [0-9]+ / [0-9]+$' <<<"$out" || { echo "!! elastic(late) 没有发生增长"; fail=1; }
grep -q '背压等待 *: 0 次' <<<"$out" || { echo "!! elastic(late) 增长后仍有阻塞"; fail=1; }

echo "================================================================"
echo "== 弹性调度：paced 稳态富余表面应被收缩"
echo "================================================================"
out=$("$BIN" --selftest openh264 --mode paced --sched elastic --pool 6 --poolmin 2 --poolmax 8 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! elastic(paced) 未通过"; fail=1
fi
grep -Eq '弹性增长/收缩/等待: [0-9]+ / [1-9][0-9]* / [0-9]+$' <<<"$out" || { echo "!! elastic(paced) 没有发生收缩"; fail=1; }

echo "================================================================"
echo "== 分辨率切换：旧代次迟到归还必须按代次隔离"
echo "================================================================"
out=$("$BIN" --selftest openh264 --scenario switch 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! switch 未通过"; fail=1
fi
grep -Eq '跨代次迟到归还 *: [1-9][0-9]*$' <<<"$out" || { echo "!! switch 没有构造出跨代次迟到归还"; fail=1; }

echo "================================================================"
echo "== 轨迹重放：记录的故障调度必须可逐事件复跑"
echo "================================================================"
out=$("$BIN" --selftest openh264 --scenario replay 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! replay 未通过"; fail=1
fi
grep -q '轨迹一致 *: 是' <<<"$out" || { echo "!! replay 两次轨迹不一致"; fail=1; }

echo "================================================================"
echo "== A/B 轨迹对照：产包优先 vs 归还优先"
echo "================================================================"
out=$("$BIN" --selftest openh264 --scenario ab --mode late 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -ne 0 ] || ! grep -q '^RESULT *: PASS$' <<<"$out"; then
  echo "!! ab 未通过"; fail=1
fi

echo "================================================================"
echo "== V4L2 硬件自检（本机无节点时应明确 FAIL，而不是伪造通过）"
echo "================================================================"
out=$("$BIN" --selftest v4l2 2>&1); rc=$?
printf '%s\n' "$out"
if [ $rc -eq 0 ]; then
  echo "-- 本机存在硬件 M2M 节点，v4l2 自检通过。"
else
  if grep -q '未找到 V4L2 M2M' <<<"$out"; then
    echo "-- 无硬件：已明确报告覆盖缺口（预期行为）。"
  else
    echo "!! v4l2 自检失败，但原因不是“无设备”。"; fail=1
  fi
fi

if [ $fail -ne 0 ]; then echo "冒烟测试：失败"; exit 1; fi
echo "冒烟测试：全部通过"
