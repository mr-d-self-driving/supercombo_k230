# Benchmarks and diagnostics

This folder keeps standalone experiments out of the production runtime path.
They are not built by the default CMake target.

Build them explicitly when needed:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSUPERCOMBO_BUILD_BENCHMARKS=ON
cmake --build build -j2
```

Host-only calibration/input-warp verification does not need nncase, OpenCV, or
K230 display libraries:

```sh
cmake -S . -B /tmp/supercombo_k230_verify \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUPERCOMBO_BUILD_RUNTIME=OFF \
  -DSUPERCOMBO_BUILD_BENCHMARKS=ON
cmake --build /tmp/supercombo_k230_verify \
  --target verify_calibration_equivalence check_preprocess_parity check_ipc_abi \
           check_ncnn_output_contract bench_input_warp_overhead -j2
./verify_calibration_equivalence
./check_preprocess_parity
./check_ipc_abi
./check_ncnn_output_contract
./bench_input_warp_overhead 3000
```

Available utilities:

- `bench_nv12_to_yuv6`: CPU `NV12 512x256 -> YUV6 float` conversion timing.
- `bench_input_warp_overhead`: compares direct YUV6 packing with the calibrated
  homography `NV12 -> YUV6` input-warp path.
- `check_preprocess_parity`: deterministic `512x256 NV12 -> warped YUV6` and
  two-frame `[previous_yuv6, current_yuv6]` stacking contract check.
- `check_ipc_abi`: shared ring/latest-channel/modelState ABI round-trip check.
- `check_ncnn_output_contract`: pruned ncnn output split, recurrent-state
  carryover, parser payload gating, fallback behavior, and modelState sanity
  check.
- `verify_calibration_equivalence`: checks the pose-based online calibration
  state machine, manual/online model-input feedback policy, medmodel homography
  matrix, `transform_scale_buffer(0.5)` UV handling, and YUV6 plane order
  against independent openpilot-formula references.
- `bench_capture_nv12`: `/dev/video2` `NV12 512x256` capture timing.
- `bench_ai2d_resize`: K230 AI2D crop/resize timing experiment.
- `sequence_runner`: run a prebuilt tensor sequence through a kmodel and dump
  raw outputs.
- `probe_drm_planes`: inspect DRM planes and optional ARGB plane commit.
- `check_model_output_parser`: sanity-check one `SCODMP1` raw-output dump.
