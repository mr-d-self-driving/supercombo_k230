# Raspberry Pi 4 runtime port

Porting plan and subagent work split: [rpi4_port_plan.md](rpi4_port_plan.md).

This port keeps the K230 runtime contract but swaps the hardware-specific parts:

```text
rpi_camerad -> /dev/shm/k230_road_ai -> rpi_modeld -> /dev/shm/k230_model_state -> rpi_overlay
```

The shared frame ring is still `512x256 NV12`. The model input path is still:

```text
NV12 512x256 -> calibrated warped YUV6 -> input_imgs [12,128,256]
```

## Build

Required external artifacts on the current Raspberry Pi:

```text
ncnn source include: /home/chan/onnx_bench/ncnn-src/src
ncnn build include:  /home/chan/onnx_bench/ncnn-build-a72-no82/src
ncnn static lib:     /home/chan/onnx_bench/ncnn-build-a72-no82/src/libncnn.a
model param:         /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param
model bin:           /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin
replay input:        /home/chan/supercombo_models/replay_120.scnv12
```

The ncnn paths are:

```text
/home/chan/onnx_bench/ncnn-src/src
/home/chan/onnx_bench/ncnn-build-a72-no82/src/libncnn.a
```

Build on the Pi:

```sh
cd /home/chan/supercombo_k230_rpi
cmake -S . -B build-rpi4 \
  -DSUPERCOMBO_BUILD_RUNTIME=OFF \
  -DSUPERCOMBO_BUILD_RPI4=ON \
  -DSUPERCOMBO_BUILD_BENCHMARKS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-rpi4 \
  --target rpi_camerad rpi_modeld rpi_overlay \
           check_preprocess_parity check_ipc_abi check_ncnn_output_contract \
           verify_calibration_equivalence \
  -j$(nproc)
```

The explicit `SUPERCOMBO_BUILD_RUNTIME=OFF` is intentional. It avoids pulling in
K230 nncase/display dependencies while building the Pi-only binaries.
`SUPERCOMBO_BUILD_BENCHMARKS=ON` is recommended on the Pi because
`scripts/rpi_smoke.sh parity` and the default aggregate `check` use the portable
preprocess/IPC/calibration gates.

If ncnn moves, pass:

```sh
-DNCNN_LIBRARY=/path/to/libncnn.a
-DNCNN_INCLUDE_DIRS="/path/to/ncnn/src;/path/to/ncnn/build/src"
```

Optional install tree:

```sh
cmake --install build-rpi4 --prefix /home/chan/supercombo_k230_rpi_install
cd /home/chan/supercombo_k230_rpi_install
scripts/rpi_smoke.sh parity
scripts/rpi_smoke.sh replay
```

`scripts/rpi_smoke.sh` resolves binaries from the install/source root by default.
If you keep the Pi binaries somewhere else, point the smoke wrapper at that
directory:

```sh
RPI_RUNTIME_DIR=/path/to/runtime scripts/rpi_smoke.sh check
```

When `SUPERCOMBO_BUILD_BENCHMARKS=ON` and `SUPERCOMBO_BUILD_RPI4=ON`, the
install tree includes the parity binaries needed by the default aggregate
`check`: `check_preprocess_parity`, `check_ipc_abi`,
`check_ncnn_output_contract`, and `verify_calibration_equivalence`.

## Model

Use the pruned visualization ncnn pair:

```text
/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param
/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin
```

This model outputs:

```text
plan 4955 + lane lines 528 + lane probabilities 8 + road edges 264 + recurrent state 512 = 6267
```

`rpi_modeld` feeds only the first `5755` floats to `ModelOutputParser` and uses the last
`512` floats as recurrent state. This is intentional: passing all `6267` floats into the
full parser would falsely treat recurrent state as lead/meta/pose output.

## Smoke Tests

The common smoke commands are wrapped in:

```sh
scripts/rpi_smoke.sh model
scripts/rpi_smoke.sh profile
scripts/rpi_smoke.sh artifacts
scripts/rpi_smoke.sh parity
scripts/rpi_smoke.sh camera-probe
scripts/rpi_smoke.sh camera
scripts/rpi_smoke.sh camera-file
scripts/rpi_smoke.sh camera-replay
scripts/rpi_smoke.sh camera-real
scripts/rpi_smoke.sh synthetic
scripts/rpi_smoke.sh replay
scripts/rpi_smoke.sh manager
scripts/rpi_smoke.sh perf
scripts/rpi_smoke.sh check
```

The script writes logs to `/tmp/rpi_smoke` by default. Override paths with
`MODEL_PARAM`, `MODEL_BIN`, `REPLAY_NV12`, and `OUT_DIR`. Frame dump is off by
default for performance; enable it with `DUMP=1` or `OVERLAY_DUMP=/tmp/out.ppm`.
At the end of each pipeline smoke, the script prints a single `SMOKE result=...`
line plus the final camera/model/overlay FPS or completion lines. It also emits
machine-readable `SMOKE_METRIC` and `SMOKE_CHECK` lines for the final component
`done` records. A smoke check fails if any component reports nonzero errors, no
frames, or misses the low default FPS floor.
Overlay smoke checks also require `model_seq > 0` by default, which proves the
overlay consumed at least one `modelState` update rather than only drawing the
camera frame. Disable that only for overlay-only debugging with
`SMOKE_REQUIRE_OVERLAY_MODEL=0`.

Default smoke floors are intentionally looser than `perf`:

```text
SMOKE_MIN_CAMERA_FPS=20
SMOKE_MIN_MODEL_FPS=1
SMOKE_MIN_OVERLAY_FPS=0
SMOKE_MIN_MANAGER_CAMERA_FPS=20
SMOKE_MIN_MANAGER_MODEL_FPS=1
SMOKE_MIN_MANAGER_OVERLAY_FPS=0
SMOKE_REQUIRE_OVERLAY_MODEL=1
```

Raise these only when the Pi is in a stable performance test setup. Use `perf`
for the stricter throughput snapshot.

`artifacts` records the exact files used by runtime/performance checks:

```text
ARTIFACT kind=model_param result=ok bytes=23303 sha256=e3c588c... path=...
ARTIFACT kind=model_bin result=ok bytes=58389784 sha256=88dc469... path=...
ARTIFACT kind=ncnn_lib result=ok bytes=5633466 sha256=146cbec... path=...
ARTIFACT_CHECK result=PASS
```

The default aggregate `check` runs this first unless `CHECK_WITH_ARTIFACTS=0`.

`profile` runs `rpi_modeld` with `RPI_PROFILE_MODEL=1` and prints a single
`PROFILE_METRIC` line with average per-frame timing:

```text
PROFILE_METRIC mode=synthetic frames=40 total_ms=... warp_ms=... input_ms=... infer_ms=... output_ms=...
```

Use `PROFILE_MODEL_FRAMES=...` for longer samples, or `CHECK_WITH_PROFILE=1`
to include the same timing breakdown in `scripts/rpi_smoke.sh check`.

`parity` runs the deterministic contract checks that do not require a camera or
ncnn inference:

```text
check_preprocess_parity
check_ipc_abi
check_ncnn_output_contract
verify_calibration_equivalence
```

It prints machine-readable lines such as:

```text
PREPROCESS_PARITY result=PASS identity_max0=0.0 identity_max1=0.0 ...
IPC_ABI result=PASS header=40 ring_header=32 road_ai_frame=48 model_state=4360 ...
NCNN_OUTPUT_CONTRACT result=PASS full=6267 parser=5755 recurrent=512 ...
verify_calibration_equivalence: PASS
```

`check_preprocess_parity` verifies zero-calibration `NV12 -> YUV6` exactness,
nonzero warp activity, and two-frame `[previous_yuv6, current_yuv6]` stacking.
`check_ipc_abi` verifies shared ring dimensions/stride, latest-channel
conflation, and `modelState` round trip for plan/lane/road-edge/lead/pose,
calibration, and lateral-plan fields.
`check_ncnn_output_contract` verifies the pruned ncnn output split:
`6267 = 5755 parser payload + 512 recurrent state`, recurrent carryover,
non-pruned fallback behavior, parser/modelState sanity, and that recurrent-tail
sentinels do not leak into plan/lane/road-edge output.

`perf` runs a short performance snapshot:

```text
model default
model threads3
model input_bf16
manager no_overlay
manager overlay_headless_2fps
manager overlay_fb_2fps
```

It writes detailed logs to `/tmp/rpi_smoke/perf*.log` and prints `PERF ...`
summary lines. Use `PERF_MODEL_FRAMES=120` or `PERF_MANAGER_SEC=10` for longer,
less noisy measurements.
Run `check` and `perf` sequentially. The Pi is CPU-bound enough that running
them at the same time can make short smoke windows miss `modelState` updates and
produce misleading failures.

`perf` also applies conservative pass/fail thresholds:

```text
PERF_MIN_MODEL_FPS=8
PERF_MIN_THREADS3_FPS=12
PERF_MIN_INPUT_BF16_FPS=15
PERF_MIN_MANAGER_CAMERA_FPS=25
PERF_MIN_MANAGER_MODEL_FPS=12
PERF_MIN_MANAGER_FB_MODEL_FPS=8
```

Each check prints a `PERF_CHECK ... result=PASS|FAIL` line. Override the env
vars above when doing stricter regression testing. Failed perf cases are retried
once by default (`PERF_ATTEMPTS=2`) to avoid treating a single Pi scheduling
outlier as a regression.

The model-only default case is intentionally loose because it has shown larger
standalone variance on this Pi. Treat the manager no-overlay and headless-overlay
checks as the primary throughput gates.

`check` is the camera-less aggregate gate for the current Pi setup. It runs:

```text
artifacts
parity
model
camera synthetic
camera-file, if ffmpeg and REPLAY_NV12 are available
synthetic pipeline with DUMP=1
manager
camera-replay, if REPLAY_NV12 exists
replay with DUMP=1, if REPLAY_NV12 exists
```

It intentionally does not fail on the missing real camera path, and it does not
run `perf` by default. Add `CHECK_WITH_CAMERA_PROBE=1` when you want the same run
to record camera discovery state as an informational step. Add
`CHECK_WITH_PERF=1` when you want the aggregate run to include the performance
threshold snapshot. Each subtest writes to its own directory under
`/tmp/rpi_smoke/check`, so the logs are safe to compare after the aggregate run.
The top-level `/tmp/rpi_smoke/check/check.log` also gets `CHECK_METRIC` summary
lines for each subtest, so a quick regression scan does not require opening
every component log.
Each `check` subtest forces the camera-less/headless path by clearing camera
source/replay/display override env vars and setting `RPI_DISPLAY=0`,
`RPI_RUN_OVERLAY=1`, and `RPI_CLEAR_SHM=1`.

Useful bounds for shorter or longer checks:

```text
CHECK_CAMERA_FRAMES=30
CHECK_MODEL_FRAMES=20
CHECK_WITH_ARTIFACTS=1
CHECK_INCLUDE_CAMERA_FILE=auto
CHECK_CAMERA_FILE_FRAMES=30
CHECK_CAMERA_FILE_SOURCE_FRAMES=60
CHECK_WITH_PARITY=1
CHECK_WITH_PROFILE=0
CHECK_PROFILE_MODEL_FRAMES=30
CHECK_SYNTHETIC_CAMERA_FRAMES=120
CHECK_SYNTHETIC_MODEL_FRAMES=12
CHECK_SYNTHETIC_OVERLAY_FRAMES=5
CHECK_CAMERA_REPLAY_FRAMES=30
CHECK_REPLAY_CAMERA_FRAMES=120
CHECK_REPLAY_MODEL_FRAMES=12
CHECK_REPLAY_OVERLAY_FRAMES=5
CHECK_MANAGER_SEC=5
CHECK_MODEL_TIMEOUT_SEC=30
CHECK_INCLUDE_REPLAY=auto
CHECK_WITH_PERF=0
CHECK_PERF_MODEL_FRAMES=40
CHECK_PERF_MANAGER_SEC=4
```

Pipeline smokes run `rpi_overlay` under `timeout` when GNU coreutils is
available. Override the default 20 seconds with `OVERLAY_TIMEOUT_SEC=...`.
`rpi_modeld` is also wrapped with `timeout` in pipeline smokes so a missing or
too-short camera source fails instead of hanging forever. Override the default
60 seconds with `MODEL_TIMEOUT_SEC=...`, or use `CHECK_MODEL_TIMEOUT_SEC=...`
inside `check`.

Set `RPI_PROFILE_MODEL=1` to split modeld timing into:

```text
warp    NV12 512x256 -> calibrated warped YUV6
input   previous/current YUV6 stacking, optional BF16 input conversion, ncnn input binding
infer   ncnn extractor.extract("outputs")
output  output copy/conversion and recurrent-state split
```

All-in-one manager smoke:

```sh
cd /home/chan/supercombo_k230_rpi
RPI_CAMERA_SYNTHETIC=1 RPI_DISPLAY=0 RPI_MANAGER_MAX_SEC=10 ./rpi_manager.py \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin
```

Use `RPI_DISPLAY=0` for headless overlay composition, `RPI_DISPLAY=fb` for direct
`/dev/fb0` output, or leave it unset when running `rpi_overlay` directly for
OpenCV HighGUI in a desktop display session. `rpi_manager.py` and
`scripts/rpi_smoke.sh` default to `RPI_DISPLAY=0`, so SSH runs do not try to open
a HighGUI window unless explicitly requested. On the current Pi, `/dev/fb0` is
`480x320 RGB565`.

When `RPI_DISPLAY=0` and no `RPI_OVERLAY_DUMP` is requested, `rpi_overlay` uses a
no-render fast path. It still checks frame/model IPC liveness, but skips
NV12-to-BGR conversion, resize, overlay drawing, and framebuffer writes.

The Pi defaults are model-throughput oriented:

```text
RPI_DISPLAY=0
RPI_OVERLAY_FPS=2
```

Use `RPI_DISPLAY=fb RPI_OVERLAY_FPS=2 scripts/rpi_smoke.sh manager` to include
framebuffer output for visual checks. This is lower cost than 5-10 fps overlay,
but `/dev/fb0` writes can still introduce modeld stalls on this Pi.

Camera input modes:

```sh
# Real camera or video file through OpenCV
RPI_CAMERA_SOURCE=/dev/video0 ./rpi_camerad
RPI_CAMERA_SOURCE=/path/to/video.mp4 ./rpi_camerad

# Synthetic 512x256 NV12 smoke source
RPI_CAMERA_SYNTHETIC=1 ./rpi_camerad

# SCNV12 replay file through the same shared frame ring
RPI_CAMERA_REPLAY_NV12=/home/chan/supercombo_models/replay_120.scnv12 ./rpi_camerad
RPI_CAMERA_REPLAY_NV12=/home/chan/supercombo_models/replay_120.scnv12 \
  RPI_CAMERA_REPLAY_LOOP=1 ./rpi_camerad
```

`scripts/rpi_smoke.sh camera-file` validates the OpenCV `VideoCapture` source
path without physical camera hardware. It uses `ffmpeg` to convert
`REPLAY_NV12` into a short MJPEG AVI under `OUT_DIR`, then runs `rpi_camerad`
with `RPI_CAMERA_SOURCE` pointing at that file.

For automated visual verification, `rpi_overlay` can dump its final rendered
BGR frame as a PPM:

```sh
RPI_DISPLAY=fb RPI_OVERLAY_DUMP=/tmp/rpi_overlay_dump.ppm ./rpi_overlay
```

Model-only synthetic:

```sh
cd /home/chan/supercombo_k230_rpi
SUPERCOMBO_MAX_FRAMES=60 RPI_SYNTHETIC=1 RPI_SYNTHETIC_FRAMES=60 \
  ./rpi_modeld \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin
```

Synthetic camera only:

```sh
SUPERCOMBO_MAX_FRAMES=30 RPI_CAMERA_SYNTHETIC=1 RPI_CAMERA_FPS=30 ./rpi_camerad
```

End-to-end headless:

```sh
rm -f /dev/shm/k230_* /dev/shm/k230*
SUPERCOMBO_MAX_FRAMES=1000 RPI_CAMERA_SYNTHETIC=1 RPI_CAMERA_FPS=30 ./rpi_camerad &
cam=$!
sleep 0.5
SUPERCOMBO_MAX_FRAMES=120 ./rpi_modeld \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin &
model=$!
sleep 0.2
SUPERCOMBO_MAX_FRAMES=60 RPI_DISPLAY=0 RPI_OVERLAY_FPS=2 ./rpi_overlay
wait $model
kill $cam 2>/dev/null || true
```

Framebuffer overlay smoke:

```sh
rm -f /dev/shm/k230_* /dev/shm/k230*
SUPERCOMBO_MAX_FRAMES=120 RPI_CAMERA_SYNTHETIC=1 RPI_CAMERA_FPS=30 ./rpi_camerad &
cam=$!
sleep 0.5
SUPERCOMBO_MAX_FRAMES=60 ./rpi_modeld \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin &
model=$!
sleep 0.2
SUPERCOMBO_MAX_FRAMES=30 RPI_DISPLAY=fb RPI_OVERLAY_FPS=2 ./rpi_overlay
wait $model
kill $cam 2>/dev/null || true
```

Replay-file overlay smoke:

```sh
rm -f /dev/shm/k230_* /dev/shm/k230*
SUPERCOMBO_MAX_FRAMES=240 \
  RPI_CAMERA_REPLAY_NV12=/home/chan/supercombo_models/replay_120.scnv12 \
  RPI_CAMERA_REPLAY_LOOP=1 \
  RPI_CAMERA_FPS=30 ./rpi_camerad &
cam=$!
sleep 0.5
SUPERCOMBO_MAX_FRAMES=80 ./rpi_modeld \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin &
model=$!
sleep 0.2
SUPERCOMBO_MAX_FRAMES=40 \
  RPI_DISPLAY=fb \
  RPI_OVERLAY_FPS=10 \
  RPI_OVERLAY_DUMP=/tmp/rpi_overlay_dump.ppm \
  ./rpi_overlay
wait $model
kill $cam 2>/dev/null || true
```

## Measured Results

On Raspberry Pi 4, Cortex-A72, OpenCV 4.10.0, ncnn BF16:

| Test | Result |
| --- | --- |
| `rpi_modeld` synthetic, 60 frames | `19.58 fps`, errors `0`, mostly `49-50 ms/frame` |
| `rpi_camerad` synthetic, 30 frames | `30.84 fps`, errors `0` |
| `rpi_camerad` synthetic + `rpi_modeld`, 180 model frames | model `19.13 fps`, errors `0` |
| `rpi_camerad` + `rpi_modeld` + headless `rpi_overlay` 10 fps | model `17.84 fps`, overlay `~10 fps`, errors `0` |
| `rpi_manager.py` + synthetic camera + framebuffer overlay, 10 sec | camera `29.13 fps`, model `17.76 fps`, overlay `~9.6 fps`, errors `0` |
| same as above with `RPI_NCNN_THREADS=3` | model `16.86 fps`, slower than default 4 threads |
| `SCNV12` replay camera + model + framebuffer overlay | camera `29.34 fps`, model `17.67 fps`, overlay `~10 fps`, errors `0` |
| short replay render with `RPI_OVERLAY_DUMP` | PPM dump generated, nonblank RGB means `[76.70, 98.13, 100.02]` |
| `scripts/rpi_smoke.sh replay` without dump | camera `29.36 fps`, model `17.36 fps`, overlay `~10 fps`, errors `0` |
| `DUMP=1 scripts/rpi_smoke.sh replay` short smoke | `overlay.ppm` generated, errors `0` |
| `DUMP=1 scripts/rpi_smoke.sh replay` with dump verification | PPM `480x320`, mean `94.44`, min `0`, max `255`, errors `0` |
| `scripts/rpi_smoke.sh parity` | preprocess parity, IPC ABI/modelState round trip, ncnn output contract, and calibration equivalence all `PASS` |
| short `scripts/rpi_smoke.sh check` with parity/profile | parity `PASS`, camera `39.36 fps`, camera-file `61.96 fps`, synthetic pipeline camera `30.24 fps`, model `12.97 fps`, overlay consumed `model_seq=6`, profile total `59.41 ms` |
| post-ncnn-contract short `scripts/rpi_smoke.sh check` with parity/profile | parity `PASS` including `NCNN_OUTPUT_CONTRACT`, camera `39.42 fps`, camera-file `76.84 fps`, synthetic pipeline camera `30.43 fps`, model `9.99 fps`, overlay consumed `model_seq=6`, profile total `68.77 ms` |
| install-tree `scripts/rpi_smoke.sh parity` and short `check` from `/tmp/supercombo_k230_rpi_install_test` | parity `PASS`, synthetic pipeline camera `29.83 fps`, model `11.32 fps`, manager camera `29.25 fps` / model `12.45 fps`, profile total `67.73 ms`; installed `rpi_*` paths used |
| install-tree `scripts/rpi_smoke.sh artifacts` | `PASS`; model param sha256 `e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2`, model bin sha256 `88dc46956eb5255265c9695a29dc4fba7ec6e419e5af26de137df756c3ec277b`, ncnn lib sha256 `146cbec8f846e9d51b0c5f63dc2f5aba031804f3c8fc67677012c1455f1ef9ed` |
| install-tree short `scripts/rpi_smoke.sh check` with artifacts/parity/profile | artifacts `PASS`, parity `PASS`, camera `39.46 fps`, camera-file `85.17 fps`, synthetic pipeline camera `29.71 fps`, model `11.25 fps`, overlay consumed `model_seq=6`, manager camera `29.29 fps` / model `14.35 fps`, profile total `69.27 ms` |
| post-camera-bound short `scripts/rpi_smoke.sh check` | artifacts `PASS`, parity `PASS`, camera `39.43 fps`, camera-file `85.21 fps`, synthetic pipeline camera `29.62 fps`, model `10.59 fps`, overlay consumed `model_seq=6`, manager camera `29.69 fps` / model `10.19 fps`; `rpi_camerad` sha256 `a2892d68df12262a741235161c3198a61d9cc6cfc4f42449a6e835e5deb1de4e` |
| `scripts/rpi_smoke.sh camera-real` with no visible USB/CSI camera | bounded failure: `CAMERA_AUTO_SOURCE result=FAIL reason=no_candidate`, all Raspberry Pi helper capture nodes remain `candidate=0` |
| `RPI_CAMERA_SOURCE=/dev/video14 CAMERA_REAL_TIMEOUT_SEC=5 scripts/rpi_smoke.sh camera-real` | bounded failure on helper node: `timeout` rc `124`, `frames=0`, `errors=1` |
| `RPI_PROFILE_MODEL=1` model-only steady state | warp `~1.8 ms`, input `~1.1 ms`, ncnn infer `~46.5 ms`, output `~0.02 ms` |
| `RPI_PROFILE_MODEL=1` replay pipeline | model `18.79 fps`, warp `1.79 ms`, input `1.15 ms`, ncnn infer `47.61 ms`, output `0.02 ms` |
| manager, overlay off | camera `29.09 fps`, model `18.70 fps`, errors `0` |
| manager, fb overlay `10 fps` | camera `29.15 fps`, model `15.72 fps`, overlay `94 frames`, errors `0` |
| manager, fb overlay `5 fps` | camera `29.12 fps`, model `15.41 fps`, overlay `38 frames`, errors `0` |
| manager, fb overlay `2 fps` | camera `29.13 fps`, model `18.54 fps`, overlay `15 frames`, errors `0` |
| manager, fb overlay uncapped | camera `29.06 fps`, model `16.31 fps`, overlay `226 frames`, errors `0` |
| `scripts/rpi_smoke.sh perf`, 40 model frames / 4 sec manager | model default `19.50 fps`, threads3 `16.76 fps`, input BF16 `19.46 fps` |
| same perf snapshot, manager no overlay | camera `29.21 fps`, model `19.37 fps`, errors `0` |
| same perf snapshot, manager headless overlay `2 fps` | camera `29.10 fps`, model `18.30 fps`, overlay `8 frames`, errors `0` |
| same perf snapshot, manager fb overlay `2 fps` | camera `29.20 fps`, model `11.89 fps`, overlay `8 frames`, errors `0`; observed framebuffer stall |
| headless no-render overlay smoke, 6 sec | `rpi_overlay: headless no-render mode`; camera `29.23 fps`, model `14.24 fps` in a run with non-overlay modeld spikes |
| no-overlay comparison, 6 sec | camera `29.06 fps`, model `18.12 fps`; also observed non-overlay `231 ms` modeld spike |
| post-fast-path perf snapshot, 40 model frames / 4 sec manager | model default `19.88 fps`, no-overlay manager `18.10 fps`, headless overlay `18.56 fps`, fb overlay `18.37 fps` |
| affinity split, `modeld=cores 1,2,3`, `camerad=core 0`, `threads=3` | camera `29.52 fps`, model `17.12 fps`; slower than default |
| affinity split, `modeld=cores 1,2,3`, `threads=4` | camera `29.73 fps`, model `12.50 fps`; much slower |
| `performance` governor, model-only 80 frames | model `19.34 fps`, no improvement over ondemand |
| `performance` governor, manager no-overlay 8 sec | camera `29.11 fps`, model `18.33 fps`, temp rose to `71.5C`; restored to `ondemand` |
| post-parity-refactor `scripts/rpi_smoke.sh perf`, 40 model frames / 4 sec manager | default model retry `18.02 fps`, threads3 `15.45 fps`, input BF16 `18.69 fps`, no-overlay manager camera `29.16 fps` / model `18.19 fps`, headless overlay model `14.97 fps`, fb overlay model `16.90 fps`; all checks `PASS` |
| post-ncnn-contract `scripts/rpi_smoke.sh perf`, 40 model frames / 4 sec manager | default model retry `8.18 fps` after a cold outlier, threads3 `16.24 fps`, input BF16 `19.07 fps`, no-overlay manager camera `29.13 fps` / model `18.22 fps`, headless overlay model `17.11 fps`, fb overlay model `15.27 fps`; all checks `PASS` |

`missed` frames in modeld are expected because the camera publishes at about 30 fps while
the CPU model consumes about 18-20 fps using latest/conflate behavior.

Default ncnn settings keep `RPI_NCNN_THREADS=4`, BF16 storage on, packing on, and
BF16 input conversion off. Three ncnn threads leave more CPU room for overlay,
but measured model throughput was lower, so 4 threads remains the best current
default.

No CPU-affinity split is enabled by default. `rpi_manager.py` supports
`RPI_MODELD_CORES`, `RPI_CAMERAD_CORES`, and `RPI_OVERLAY_CORES` for experiments,
and logs the actual child CPU mask/nice at startup. The measured split-core
configurations were slower than the default all-core modeld run.

The current Pi bottleneck is ncnn inference, not NV12 warp/YUV6 packing. After
the first frame builds the warp map, preprocessing stays around `3 ms/frame`
combined, while ncnn inference stays around `46-48 ms/frame`.
Framebuffer overlay can still steal enough CPU/memory bandwidth to cause modeld
spikes, so the default display path is headless and the overlay cap is `2 fps`.

Quick ncnn option sweep, 50 synthetic frames:

| Config | FPS |
| --- | ---: |
| default before input change, `threads=4`, BF16 input on | `18.62` |
| `threads=3` | `17.27` |
| `threads=2` | `13.14` |
| `threads=4`, `RPI_NCNN_SGEMM=1` | `18.68` |
| `threads=4`, `RPI_NCNN_PACKING=0` | `12.41` |
| `threads=4`, `RPI_NCNN_INPUT_BF16=0` | `19.00` |
| `threads=4`, `RPI_NCNN_BF16=0`, `RPI_NCNN_INPUT_BF16=0` | `9.18` |

Longer 120-frame confirmation was `19.41 fps` with BF16 input conversion and
`19.53 fps` without it, so the default now skips input BF16 conversion.

## Current Real Camera Status

The USB camera was detected as:

```text
Web Camera: Web Camera (usb-0000:01:00.0-1.2)
  /dev/video0
  /dev/video1
```

But `rpi_camerad` and `v4l2-ctl` hit UVC timeouts. After that, `lsusb` no longer
listed the camera and `rpicam-vid --list-cameras` reported no CSI cameras. `dmesg`
showed:

```text
Failed to query UVC control ... -110
device descriptor read/64, error -32
device not accepting address, error -71
unable to enumerate USB device
```

So the current code path is ready for V4L2/OpenCV camera input, but the attached USB camera
or hub state must be fixed before real-camera smoke can pass.

Latest probe on the Pi:

```text
lsusb: only Linux root hub and VIA hub are listed
rpicam-vid --list-cameras: No cameras available
CAMERA_PROBE_V4L2 candidates=0
CAMERA_PROBE result=FAIL usb_candidate=0 csi_candidate=0 v4l2_candidates=0
```

The visible `/dev/video*` nodes are Raspberry Pi codec/ISP/helper nodes. Some of
them advertise `Video Capture`, but `camera-probe` marks them as
`candidate=0 reason=platform_or_helper_capture`, not as usable live camera input
for `rpi_camerad`.

Kernel log still shows the original UVC failure path:

```text
Found UVC 1.00 device Web Camera (1d6c:1278)
Failed to query UVC control ... -110
device descriptor read/64, error -32
device not accepting address ..., error -71
unable to enumerate USB device
```

Once a camera is visible again, run:

```sh
scripts/rpi_smoke.sh camera-probe
scripts/rpi_smoke.sh camera-real
```

`camera-probe` is expected to fail while no USB UVC or CSI camera is visible.
It records `lsusb`, V4L2 device listing, `rpicam-vid --list-cameras`, and recent
camera/USB kernel messages to `/tmp/rpi_smoke/probe.log`.
It also prints `CAMERA_PROBE_NODE` lines for every `/dev/video*` node. Use nodes
with `candidate=1` as `RPI_CAMERA_SOURCE=/dev/videoX`. Raspberry Pi codec/ISP
helper nodes can expose `Video Capture` capabilities but are reported as
`candidate=0 reason=platform_or_helper_capture`, so do not use those as the live
camera source for this OpenCV path.
`camera-real` now uses the first `candidate=1` V4L2 node automatically when
`RPI_CAMERA_SOURCE` is not set. Set `RPI_CAMERA_SOURCE=/dev/videoX` only when
you want to override the probe result.
Live camera reads are bounded so a helper node that opens but never produces
frames cannot hang the smoke run forever. `rpi_camerad` stops after
`RPI_CAMERA_MAX_READ_ERRORS=90` consecutive failed reads by default, and
`camera-real` is wrapped by `CAMERA_REAL_TIMEOUT_SEC=15` unless set to `0`.
