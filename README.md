# 硬编接入实验台（Encoder Lab）

一个可运行的硬件视频编码接入原型：程序自己生成帧，按设备的真实要求建立图像表面
（颜色格式 / 平面步幅 / 有效尺寸都做真实转换），提交给编码接口，异步取回码流并用
独立解码器还原画面逐帧核对。仓库同时带一个软件对照后端（OpenH264），在没有硬件
编码节点的主机上也能跑通并演示同一套接口语义；它不会被包装成硬件测量。

## 1. 构建

依赖：C++17 编译器、GNU make、zlib 开发文件。OpenH264 2.4.1 已随仓库提供
（`third_party/openh264/`，含 `LICENSE` 与 `BUILD_INFO.txt`），无需联网。

```sh
make
```

产物是 `build/encoder_lab`。

## 2. 启动界面

```sh
./build/encoder_lab --port 8099
```

然后在浏览器打开：

```
http://127.0.0.1:8099/
```

可选参数：

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | `127.0.0.1` | 监听地址；只在本机使用时保持默认 |
| `--port` | `8099` | 监听端口 |
| `--web` | `web` | 静态界面目录 |

服务启动后，界面由硬编接入内核（`Session`）的状态驱动：可用的编码后端、协商后的
表面格式、当前送帧方式的能力与限制都来自后端探测结果，而不是写死在页面里。运行
结束后按 `Ctrl-C` 退出（会先停止正在运行的任务并回收资源）。

## 3. 在界面上做什么

1. 选择 **编码后端**：`OpenH264 软件对照（接口模型）` 或探测到的 V4L2 M2M 节点
   （若本机没有硬件节点，下拉列表里只有软件对照，并在上方明确标注硬件未覆盖）。
2. 选择 **送帧方式**，点「开始运行」：

   | 方式 | 演示的接口语义 |
   | --- | --- |
   | `paced` | 按帧率节流，正常完成回执与回收 |
   | `burst` | 不节流，尽快提交，观察有限池上的背压 |
   | `late` | 延迟完成回执，表面在码流可取后仍保持不可写，直到回执才复用 |
   | `guard` | 所有权守卫：对仍可能被硬件读取的表面尝试写入会被拒绝并计数 |
   | `corrupt` | 注入一次表面内容损坏，验证解码核对能检出而不是静默通过 |

3. **结果区**并排显示源画面与解码输出，并给出三个相互独立、绝不合并的判定标签：

   * `会话创建` —— 编码会话是否创建成功；
   * `链路` —— 每个提交帧是否都产出码流、解码画面在帧号 / 参考色块 / 颜色 / 内容上
     是否与源一致、所有表面是否归还；
   * `硬件覆盖` —— 本次是否真的跑在硬件编码器上；软件对照会明确写成未覆盖。

4. **表面状态** 区显示每个表面（含索引、状态 FREE/FILLING/QUEUED/RETIRED、当前帧号、
   复用次数、填充次数、实际协商出的格式 / 可见尺寸 / 编码尺寸 / 步幅、释放条件）。
   表面池是有限的：在途数量受池大小约束，不需要靠无限分配掩盖回收问题。
5. **帧号 / 顺序 / 回执** 表逐帧列出状态、所用表面、码流大小、标记期望/实际、色块与
   颜色核对、MAE、提交→码流、提交→可复用两个时延，以及异常说明。
6. **运行指标** 包含提交/码流/核对计数、乱序与重复、背压等待次数与累计时间、最大在途
   表面、未归还表面、EOS 排空状态、RSS 基线与峰值、时延分位数。

界面每 250 ms 轮询一次 `/api/state`；页面本身不持有运行状态，刷新不会丢失结果。

## 4. 无浏览器自检

用于 CI 或远程主机上验证整条链路：

```sh
./build/encoder_lab --selftest openh264            # 默认 paced
./build/encoder_lab --selftest openh264 --mode late
./build/encoder_lab --selftest v4l2                # 无硬件时诚实 FAIL
```

自检会打印中文报告（提交/码流数、帧号核对、背压、最大在途、未归还表面、EOS、
RSS、时延），最后一行是 `RESULT : PASS` 或 `RESULT : FAIL`，退出码 0/1。

在没有匹配的 V4L2 M2M H.264 编码节点时，v4l2 自检会明确报告「未找到设备、不报告
任何硬件吞吐数据」并返回 FAIL —— 这是覆盖缺口，而不是被软件结果掩盖的通过。

## 5. HTTP 接口

| 方法与路径 | 说明 |
| --- | --- |
| `GET /api/state` | 当前运行快照：配置、表面、逐帧结果、指标、判定 |
| `GET /api/devices` | 探测到的 V4L2 节点与软件对照后端 |
| `POST /api/start?...` | 启动一次运行，参数见下 |
| `POST /api/stop` | 请求停止并排空 |
| `GET /api/frame.png?kind=src\|out&seq=N&w=360` | 源画面 / 解码输出 PNG |
| `GET /api/au?seq=N` | 该帧 H.264 访问单元（响应头带 `X-Keyframe`、`X-Frame-Seq`） |

`/api/start` 查询参数：`backend`、`mode`、`width`、`height`、`fps`、`bitrate`、`gop`、
`pool`、`frames`、`verify`、`receiptLagMs`、`device`、`format`。

例：

```sh
curl -s 'http://127.0.0.1:8099/api/start?backend=openh264&mode=burst&frames=30&pool=3' 
curl -s http://127.0.0.1:8099/api/state | python3 -m json.tool
```

## 6. 硬件接入说明（V4L2 M2M）

`src/encoder_v4l2.cpp` 走标准的 V4L2 内存到内存接口：`QUERYCAP` → `ENUM_FMT` →
`S_FMT/G_FMT`（尊重驱动实际授予的编码尺寸与 `bytesperline`）→ `REQBUFS/QUERYBUF/mmap`
→ `QBUF/DQBUF` 双队列 → `STREAMON` → `VIDIOC_ENCODER_CMD(V4L2_ENC_CMD_STOP)` 排空 →
`V4L2_BUF_FLAG_LAST` → `STREAMOFF` 与 `munmap`。同时支持单平面与多平面（`_MPLANE`）
节点，并把驱动返回的时间戳映射回帧号（无时间戳复制时按 FIFO 顺序回退并记录说明）。

当前环境没有可用的硬件编码节点，因此仓库内没有、也不会提供任何硬件测速数据。有
设备时，界面会把该节点列入后端下拉框，`硬件覆盖` 标签才会变绿。

## 7. 文件结构

```
src/
  image.*        颜色转换、平面布局、标记/色块生成与核对
  encoder.h      后端抽象（能力、表面、事件、可写性判定）
  encoder_sw.cpp OpenH264 软件对照后端
  encoder_v4l2.cpp 真实 V4L2 M2M 后端
  decoder.h      独立 OpenH264 解码器，用于核对
  session.*      有限表面池、送帧循环、回执/回收记账、指标
  server.cpp     HTTP/JSON 服务与静态文件
  png.cpp        PNG 编码（zlib）
  main.cpp       CLI 与自检入口
web/             界面（含内置 CJK 子集字体，离线可用）
web/fonts/LICENSE.txt  Noto Sans CJK SC 子集字体的 SIL OFL 1.1 许可
third_party/openh264/  预编译的 OpenH264 2.4.1 静态库与头文件
scripts/         冒烟与 API 测试
```

## 8. 已知限制

* 本环境没有硬件编码节点，`encoder_v4l2.cpp` 的代码路径无法在此实测；它按标准
  V4L2 M2M 语义编写，但未在本机设备上验证过。界面与自检会明确标为「硬件未覆盖」。
* 软件对照后端是 OpenH264，它的“延迟回执”是按接口模型模拟的（码流同步可得、
  表面在回执时刻才释放），用于演示回收语义，不代表任何硬件的时延或吞吐。
* V4L2 后端在驱动不回报时间戳时按 FIFO 顺序把输出映射回帧号，并在日志中说明；
  若驱动乱序返回且不支持时间戳复制，帧号映射会不准确，这一点不会被静默忽略。

## 9. 测试

```sh
scripts/smoke.sh        # 构建 + 五种送帧方式的自检
scripts/api_test.sh     # 启动服务并通过 HTTP 验证一次完整运行
```

`smoke.sh` 对每种方式断言 `RESULT : PASS`（`guard` 方式要求拒绝计数 ≥ 1 且无写入
损坏；`corrupt` 方式要求恰好检出 1 帧）。`api_test.sh` 会校验：会话创建、逐帧码流、
解码核对、零未归还表面、EOS 完成，以及 `/api/frame.png`、`/api/au` 可实际取到数据。
