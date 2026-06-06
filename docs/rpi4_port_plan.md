# Raspberry Pi 4 Port Plan

## Goal

Bring up a Raspberry Pi 4 compatible runtime while the K230 board is unavailable.
The port must preserve the current K230 inference contract wherever practical:

```text
camera source -> 512x256 NV12 shared ring -> modeld -> modelState -> overlay
```

The model input contract remains:

```text
512x256 NV12 -> calibrated warped YUV6 -> input_imgs [12,128,256]
```

The Pi runtime uses the local ncnn CPU BF16 artifacts from:

```text
/Users/chan/Documents/RPI4/ncnn_cpu_bf16_results
```

The RPi path is a compatibility and validation target, not the final hardware
target. K230-specific behavior should be preserved unless a Pi replacement is
explicitly isolated behind an RPi-only target.

## Current Subagent Findings

The first planning pass assigned independent read-only explorers before making
the plan concrete. Their findings are part of the porting baseline:

- K230 crop/resize is done by the K230 V4L2/DRM capture path, not by CPU code in
  `k230_camerad`.
- K230 `k230_overlay` owns `/dev/video1` preview and a DRM `ARGB8888` overlay
  plane; it does not consume the AI NV12 ring for preview.
- The shared-memory ABI is fixed around `/k230_road_ai_frame`,
  `/k230_road_ai`, `/k230_model_state`, four `512x256 NV12` slots, and
  latest-frame consumption.
- RPi display is intentionally different: `rpi_overlay` reads the AI ring and
  renders a low-FPS/headless validation view. This must not be treated as the
  K230 display architecture.
- The RPi ncnn artifact directory is a benchmark/reproducer source, not a full
  deploy bundle. The deploy pair is expected as external
  `supercombo_no_big_drop_pruned_viz_opt.{param,bin}` files.
- The pruned ncnn output is `6267` floats: the first `5755` feed
  `ModelOutputParser`, and the last `512` are recurrent state.
- `rpi_camerad` metadata is not yet equivalent to K230 crop metadata. Downstream
  code must not rely on those metadata fields for calibration/provenance until
  they are fixed and tested.

## Subagent Strategy

Use subagents aggressively, but keep their ownership disjoint. The main agent is
the integrator and owns commits, final test runs, and conflict resolution.

| Role | Responsibility | Output |
| --- | --- | --- |
| Main integrator | Owns branch, edits shared contracts, runs final build/smoke/perf, commits | Integrated patches and verified results |
| K230 contract explorer | Inspect K230 camera, resize/crop, shared ring, model input, and overlay paths | File/function map and invariants that the RPi port must keep |
| RPi model explorer | Inspect ncnn BF16 artifacts and Pi build/runtime assumptions | Model ABI, artifact paths, CMake requirements, expected performance commands |
| Verification explorer | Compare K230 and RPi runtime contracts and propose regression gates | Test matrix, acceptance thresholds, risk list |
| Camera worker | Owns `src/rpi_camerad.cc` and camera-source adapters only | Pi camera/file/replay capture implementation |
| Model worker | Owns `src/ncnn_supercombo_model.*` and `src/rpi_modeld.cc` only | ncnn model execution and timing/profile support |
| Overlay/docs worker | Owns `src/rpi_overlay.cc`, `scripts/rpi_smoke.sh`, and docs only | Display smoke, visual dump, and runbook updates |

Subagents must not edit the same files in parallel unless the main integrator
explicitly reassigns ownership. Shared IPC files such as `src/k230_ipc.*` are
main-integrator-owned because they affect both K230 and RPi paths.

## Work Plan

### 1. Branch and Baseline

- Create a dedicated branch from current `main`.
- Commit or stash unrelated local changes before porting.
- Record the existing K230 runtime shape and the RPi artifact locations.
- Confirm CMake can build the existing K230 targets unchanged after the branch.

### 2. K230 Runtime Contract Extraction

Subagent: K230 contract explorer.

Map the exact K230 path:

- camera input and crop/resize
- `512x256 NV12` frame publishing
- shared memory frame ring layout
- `NV12 -> calibrated warped YUV6`
- model output parsing into `modelState`
- overlay drawing and replay/debug paths

Acceptance output:

- exact files/functions
- invariants that must be retained
- hardware-specific code that must not leak into the RPi target

Known K230 reference points:

- `k230_manager.py`: starts display/overlay first and uses display readiness to
  avoid K230 preview startup races.
- `src/k230_camerad.cc`, `src/input_source.cc`: live K230 AI capture path and
  hardware crop/resize to `512x256 NV12`.
- `src/k230_ipc.*`: shared frame ring, model-state IPC, and compact parsed
  state layout.
- `src/k230_modeld.cc`, `src/supercombo_model.cc`,
  `src/model_input_transform.cc`: ring consumption and calibrated
  `NV12 -> warped YUV6` input.
- `src/k230_overlay.cc`, `src/overlay_renderer.cc`: K230 preview/DRM overlay and
  portable road overlay drawing.

### 3. RPi Model Artifact and ABI Check

Subagent: RPi model explorer.

Inspect the optimized ncnn artifacts and define the RPi model ABI:

- `.param` and `.bin` names
- input tensor name/shape/type
- output tensor name/layout
- recurrent-state handling
- required ncnn include/library paths
- preferred ncnn options on Cortex-A72

Acceptance output:

- CMake variables for ncnn
- runtime arguments and default paths
- known model output size and parser boundary

Artifact provenance gate:

- Confirm whether the local artifact path contains deployable `.param/.bin`
  files or only conversion inputs.
- Record SHA256 for any `.onnx`, `.param`, and `.bin` files used for
  performance numbers.
- Preserve the conversion chain in the notes:
  `supercombo.onnx -> no_big_input ONNX -> pruned viz ONNX -> onnx2ncnn -> ncnnoptimize`.
- Do not compare performance runs unless they use the same optimized ncnn pair.

### 4. RPi Process Skeleton

Implement Pi-specific binaries without changing the K230 binaries:

```text
rpi_camerad -> /dev/shm/k230_road_ai
rpi_modeld  -> /dev/shm/k230_model_state
rpi_overlay -> optional framebuffer/headless visual check
rpi_manager.py -> supervision for smoke runs
```

Keep the shared ring names compatible so tests can reuse the existing IPC
helpers and model parsing code.

### 5. Camera/Input Port

Subagent: Camera worker.

Implement Pi capture modes:

- OpenCV `VideoCapture` device path, for real camera when available
- video-file source for camera-less testing
- synthetic NV12 source
- SCNV12 replay source

Every mode must publish exactly `512x256 NV12` frames into the shared ring.
Real camera quality can wait; the first gate is deterministic frame delivery.

### 6. Model Port

Subagent: Model worker.

Implement the ncnn CPU BF16 runner:

- load `.param/.bin`
- build calibrated warped YUV6 input
- maintain previous/current input history
- run ncnn inference
- split recurrent state from parsed model output
- publish compact `modelState`

Add profiling for:

```text
warp, input, infer, output, total
```

The profile result should be machine-readable so it can be used in regression
checks.

### 7. Overlay and Debug Output

Subagent: Overlay/docs worker.

Provide low-risk visualization:

- headless IPC liveness mode
- optional framebuffer output
- optional PPM dump for automated visual verification
- low default overlay FPS so modeld throughput is not hidden by display cost

RPi overlay is a validation tool. It should not dictate K230 display design.

### 8. Verification Matrix

Subagent: Verification explorer, then main integrator executes final checks.

Deterministic parity gates before runtime smoke:

- Preprocessing parity: feed a fixed `SCNV12` or synthetic NV12 fixture through
  `ModelInputTransform` and compare the resulting `input_imgs` against a
  checked reference. Include zero calibration and at least one nonzero
  pitch/yaw case. Pass/fail should report max absolute error, mean absolute
  error, and a checksum for the final `[12,128,256]` tensor.
- Two-frame history parity: verify frame 0 and frame 1 stacking so the previous
  YUV6 half of `input_imgs` is not accidentally shifted or duplicated.
- IPC ABI parity: compile-time/static checks for shared struct sizes and
  constants, plus a writer/reader smoke that validates slot stride, frame bytes,
  metadata topic, monotonic `frame_id`, and latest/conflate consumption.
- ModelState ABI parity: publish a canned parsed output through
  `publish_model_state`, read it back, and compare plan/lane/road-edge/lead,
  calibration, and lateral-plan fields.
- ncnn semantic parity: with a canned ncnn output or short replay run, assert
  output size `6267`, parser payload `5755`, recurrent tail `512`, recurrent
  carryover across two frames, and finite parsed `modelState` values.

Minimum checks:

```sh
scripts/rpi_smoke.sh model
scripts/rpi_smoke.sh profile
scripts/rpi_smoke.sh camera
scripts/rpi_smoke.sh camera-file
scripts/rpi_smoke.sh synthetic
scripts/rpi_smoke.sh manager
scripts/rpi_smoke.sh perf
scripts/rpi_smoke.sh check
```

Useful aggregate gate:

```sh
CHECK_WITH_PROFILE=1 CHECK_WITH_PERF=1 scripts/rpi_smoke.sh check
```

Raw ncnn upper-bound benchmark, when the artifact repo is present on the Pi:

```sh
cd /home/chan/onnx_bench/ncnn_cpu_bf16_results
scripts/run_three_cases.sh
scripts/rpi_bf16_fast_bench.sh \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin \
  500 100
```

Runtime profile gate matching the current best BF16 input path:

```sh
MODEL_FRAMES=80 RPI_NCNN_INPUT_BF16=1 scripts/rpi_smoke.sh model
PROFILE_MODEL_FRAMES=80 RPI_NCNN_INPUT_BF16=1 scripts/rpi_smoke.sh profile
```

Real camera gate, when a usable camera is attached:

```sh
scripts/rpi_smoke.sh camera-probe
scripts/rpi_smoke.sh camera-real
```

### 9. Performance Acceptance

The Pi is CPU-only, so it is not expected to match K230 NPU latency. The useful
criteria are stability and attribution:

- camera synthetic/replay stays near 30 fps
- modeld reports errors `0`
- modeld uses latest/conflate behavior when camera is faster than inference
- profile output separates preprocessing from ncnn inference
- overlay can be disabled or capped without affecting model correctness

The expected bottleneck is ncnn inference, not NV12 packing. If performance
regresses, first compare `PROFILE_METRIC` fields before changing architecture.

Treat framebuffer overlay results as a display-cost experiment, not as the model
throughput baseline. The baseline for inference is model-only or manager
headless/no-overlay.

## Open Risks

- RPi OpenCV capture can change FOV, chroma siting, color range, lens
  distortion, and exposure behavior relative to K230 hardware capture.
- `rpi_camerad` crop metadata is currently validation metadata only. Fix it
  before using it in calibration, logging provenance, or replay comparison.
- The current RPi overlay reads the AI ring for display, unlike K230 preview.
  This is acceptable for validation, but it can hide display-specific K230
  issues.
- The exact `ncnnoptimize` invocation for the current deploy pair must be
  recorded before a clean rebuild can be considered reproducible.
- BF16 input conversion has shown mixed results between artifact-level and
  repo-level smokes, so both default float input and `RPI_NCNN_INPUT_BF16=1`
  should remain in performance checks until the result is stable.

## Done Criteria

- RPi branch builds with CMake.
- K230 targets remain buildable and their public runtime contract is unchanged.
- RPi smoke tests pass without real camera hardware.
- RPi profile output identifies preprocessing and inference cost separately.
- Documentation records build commands, artifact paths, smoke commands, and
  measured results.
- Subagent reports have been reconciled into the plan or runbook before merge.
