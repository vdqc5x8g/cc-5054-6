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
