# Raspberry Pi 4 runtime port

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
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-rpi4 --target rpi_camerad rpi_modeld rpi_overlay -j$(nproc)
```

The explicit `SUPERCOMBO_BUILD_RUNTIME=OFF` is intentional. It avoids pulling in
K230 nncase/display dependencies while building the Pi-only binaries.

If ncnn moves, pass:

```sh
-DNCNN_LIBRARY=/path/to/libncnn.a
-DNCNN_INCLUDE_DIRS="/path/to/ncnn/src;/path/to/ncnn/build/src"
```

Optional install tree:

```sh
cmake --install build-rpi4 --prefix /home/chan/supercombo_k230_rpi_install
cd /home/chan/supercombo_k230_rpi_install
scripts/rpi_smoke.sh replay
```

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
scripts/rpi_smoke.sh camera-probe
scripts/rpi_smoke.sh camera
scripts/rpi_smoke.sh camera-replay
RPI_CAMERA_SOURCE=/dev/video0 scripts/rpi_smoke.sh camera-real
scripts/rpi_smoke.sh synthetic
scripts/rpi_smoke.sh replay
scripts/rpi_smoke.sh manager
scripts/rpi_smoke.sh perf
```

The script writes logs to `/tmp/rpi_smoke` by default. Override paths with
`MODEL_PARAM`, `MODEL_BIN`, `REPLAY_NV12`, and `OUT_DIR`. Frame dump is off by
default for performance; enable it with `DUMP=1` or `OVERLAY_DUMP=/tmp/out.ppm`.
At the end of each pipeline smoke, the script prints a single `SMOKE result=...`
line plus the final camera/model/overlay FPS or completion lines.

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

`missed` frames in modeld are expected because the camera publishes at about 30 fps while
the CPU model consumes about 18-20 fps using latest/conflate behavior.

Default ncnn settings keep `RPI_NCNN_THREADS=4`, BF16 storage on, packing on, and
BF16 input conversion off. Three ncnn threads leave more CPU room for overlay,
but measured model throughput was lower, so 4 threads remains the best current
default.

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
vcgencmd get_camera: supported=0 detected=0, libcamera interfaces=0
```

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
RPI_CAMERA_SOURCE=/dev/video0 scripts/rpi_smoke.sh camera-real
```

`camera-probe` is expected to fail while no USB UVC or CSI camera is visible.
It records `lsusb`, V4L2 device listing, `rpicam-vid --list-cameras`, and recent
camera/USB kernel messages to `/tmp/rpi_smoke/probe.log`.
