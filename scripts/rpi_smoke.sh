#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-replay}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [[ -n "${RPI_RUNTIME_DIR:-}" ]]; then
  ROOT_DIR="${RPI_RUNTIME_DIR}"
elif [[ ! -x "${ROOT_DIR}/rpi_modeld" && -x "${SCRIPT_DIR}/rpi_modeld" ]]; then
  ROOT_DIR="${SCRIPT_DIR}"
elif [[ ! -x "${ROOT_DIR}/rpi_modeld" && -x "${ROOT_DIR}/build-rpi4/rpi_modeld" ]]; then
  ROOT_DIR="${ROOT_DIR}/build-rpi4"
fi

MODEL_PARAM="${MODEL_PARAM:-/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param}"
MODEL_BIN="${MODEL_BIN:-/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin}"
NCNN_LIBRARY="${NCNN_LIBRARY:-/home/chan/onnx_bench/ncnn-build-a72-no82/src/libncnn.a}"
REPLAY_NV12="${REPLAY_NV12:-/home/chan/supercombo_models/replay_120.scnv12}"
OUT_DIR="${OUT_DIR:-/tmp/rpi_smoke}"
DISPLAY_MODE="${RPI_DISPLAY:-0}"
OVERLAY_FPS="${RPI_OVERLAY_FPS:-2}"
CAMERA_FPS="${RPI_CAMERA_FPS:-30}"

mkdir -p "${OUT_DIR}"

reset_outputs() {
  rm -f "${OUT_DIR}/camera.log" \
        "${OUT_DIR}/model.log" \
        "${OUT_DIR}/overlay.log" \
        "${OUT_DIR}/manager.log" \
        "${OUT_DIR}/artifacts.log" \
        "${OUT_DIR}/parity.log" \
        "${OUT_DIR}/perf.log" \
        "${OUT_DIR}/probe.log" \
        "${OUT_DIR}/dump_verify.log" \
        "${OUT_DIR}/overlay.ppm"
}

print_matches() {
  local label="$1"
  local path="$2"
  local pattern="$3"
  echo "=== ${label} ==="
  if [[ ! -f "${path}" ]]; then
    echo "missing log: ${path}"
    return
  fi
  tr '\r' '\n' <"${path}" | grep -E "${pattern}" | tail -n 6 || tr '\r' '\n' <"${path}" | tail -n 6
}

last_log_line() {
  local path="$1"
  local pattern="$2"
  [[ -f "${path}" ]] || return 1
  tr '\r' '\n' <"${path}" | grep -E "${pattern}" | tail -n 1
}

field_from_line() {
  local line="$1"
  local key="$2"
  sed -n "s/.*${key}=\([^ ]*\).*/\1/p" <<<"${line}"
}

number_ge() {
  local actual="$1"
  local minimum="$2"
  [[ -n "${actual}" && "${actual}" != "NA" ]] || return 1
  awk -v actual="${actual}" -v minimum="${minimum}" \
    'BEGIN { exit !((actual + 0.0) >= (minimum + 0.0)) }'
}

validate_component_log() {
  local component="$1"
  local path="$2"
  local pattern="$3"
  local min_fps="${4:-0}"

  local line
  if ! line="$(last_log_line "${path}" "${pattern}")"; then
    echo "SMOKE_CHECK component=${component} result=FAIL reason=missing_done log=${path}"
    return 1
  fi

  local frames errors fps missed model_seq have_model
  frames="$(field_from_line "${line}" frames)"
  errors="$(field_from_line "${line}" errors)"
  fps="$(field_from_line "${line}" fps)"
  missed="$(field_from_line "${line}" missed)"
  model_seq="$(field_from_line "${line}" model_seq)"
  have_model="$(field_from_line "${line}" have_model)"
  echo "SMOKE_METRIC component=${component} frames=${frames:-NA} missed=${missed:-NA} errors=${errors:-NA} fps=${fps:-NA} model_seq=${model_seq:-NA} have_model=${have_model:-NA}"

  if [[ -z "${frames}" ]] || ! number_ge "${frames}" 1; then
    echo "SMOKE_CHECK component=${component} result=FAIL reason=no_frames frames=${frames:-NA}"
    return 1
  fi
  if [[ -z "${errors}" || "${errors}" != "0" ]]; then
    echo "SMOKE_CHECK component=${component} result=FAIL reason=errors errors=${errors:-NA}"
    return 1
  fi
  if [[ "${min_fps}" != "0" && "${min_fps}" != "0.0" ]]; then
    if ! number_ge "${fps:-NA}" "${min_fps}"; then
      echo "SMOKE_CHECK component=${component} result=FAIL reason=fps actual=${fps:-NA} min=${min_fps}"
      return 1
    fi
  fi
  if [[ "${component}" == "overlay" && "${SMOKE_REQUIRE_OVERLAY_MODEL:-1}" != "0" ]]; then
    if ! number_ge "${model_seq:-NA}" 1; then
      echo "SMOKE_CHECK component=${component} result=FAIL reason=no_model_state model_seq=${model_seq:-NA}"
      return 1
    fi
  fi

  echo "SMOKE_CHECK component=${component} result=PASS"
  return 0
}

validate_pipeline_logs() {
  local rc=0
  validate_component_log camera "${OUT_DIR}/camera.log" 'rpi_camerad done frames=' "${SMOKE_MIN_CAMERA_FPS:-20}" || rc=1
  validate_component_log model "${OUT_DIR}/model.log" 'rpi_modeld (synthetic )?done frames=' "${SMOKE_MIN_MODEL_FPS:-1}" || rc=1
  validate_component_log overlay "${OUT_DIR}/overlay.log" 'rpi_overlay done frames=' "${SMOKE_MIN_OVERLAY_FPS:-0}" || rc=1
  return "${rc}"
}

validate_manager_log() {
  local rc=0
  validate_component_log camera "${OUT_DIR}/manager.log" 'rpi_camerad done frames=' "${SMOKE_MIN_MANAGER_CAMERA_FPS:-20}" || rc=1
  validate_component_log model "${OUT_DIR}/manager.log" 'rpi_modeld done frames=' "${SMOKE_MIN_MANAGER_MODEL_FPS:-1}" || rc=1
  if [[ "${RPI_RUN_OVERLAY:-1}" != "0" ]]; then
    validate_component_log overlay "${OUT_DIR}/manager.log" 'rpi_overlay done frames=' "${SMOKE_MIN_MANAGER_OVERLAY_FPS:-0}" || rc=1
  fi
  return "${rc}"
}

emit_profile_metric() {
  local path="$1"
  local line
  if ! line="$(last_log_line "${path}" 'rpi_modeld profile mode=')"; then
    echo "PROFILE_METRIC result=missing log=${path}"
    return 1
  fi
  local mode frames total warp input infer output
  mode="$(field_from_line "${line}" mode)"
  frames="$(field_from_line "${line}" frames)"
  total="$(field_from_line "${line}" total)"
  warp="$(field_from_line "${line}" warp)"
  input="$(field_from_line "${line}" input)"
  infer="$(field_from_line "${line}" infer)"
  output="$(field_from_line "${line}" output)"
  echo "PROFILE_METRIC mode=${mode:-NA} frames=${frames:-NA} total_ms=${total:-NA} warp_ms=${warp:-NA} input_ms=${input:-NA} infer_ms=${infer:-NA} output_ms=${output:-NA}"
  return 0
}

summarize_pipeline() {
  local source_mode="$1"
  local model_rc="$2"
  local overlay_rc="$3"
  local overlay_dump="$4"
  local dump_rc="$5"
  local check_rc=0
  local result="PASS"
  validate_pipeline_logs || check_rc=1
  if [[ "${model_rc}" -ne 0 || "${overlay_rc}" -ne 0 || "${dump_rc}" -ne 0 || "${check_rc}" -ne 0 ]]; then
    result="FAIL"
  fi
  print_matches camera "${OUT_DIR}/camera.log" 'rpi_camerad (fps|done)'
  print_matches model "${OUT_DIR}/model.log" 'rpi_modeld(:| .*done| synthetic| replay)'
  print_matches overlay "${OUT_DIR}/overlay.log" 'rpi_overlay (fps|done)'
  if [[ -n "${overlay_dump}" ]]; then
    if [[ -f "${overlay_dump}" ]]; then
      ls -l "${overlay_dump}"
    else
      echo "dump missing: ${overlay_dump}"
    fi
  fi
  echo "SMOKE result=${result} mode=${source_mode} model_rc=${model_rc} overlay_rc=${overlay_rc} dump_rc=${dump_rc} check_rc=${check_rc} dump=${overlay_dump:-none} logs=${OUT_DIR}"
  return "${check_rc}"
}

verify_ppm_dump() {
  local path="$1"
  if [[ "${VERIFY_DUMP:-1}" == "0" || -z "${path}" ]]; then
    return 0
  fi
  python3 - "$path" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.is_file():
    print(f"dump verify failed: missing {path}")
    raise SystemExit(1)

data = path.read_bytes()
parts = data.split(b"\n", 3)
if len(parts) != 4 or parts[0] != b"P6":
    print(f"dump verify failed: bad PPM header {path}")
    raise SystemExit(1)
try:
    width, height = map(int, parts[1].split())
    max_value = int(parts[2])
except Exception:
    print(f"dump verify failed: bad PPM metadata {path}")
    raise SystemExit(1)
pixels = parts[3]
expected = width * height * 3
if max_value != 255 or width <= 0 or height <= 0 or len(pixels) != expected:
    print(f"dump verify failed: size mismatch {path} {width}x{height} bytes={len(pixels)} expected={expected}")
    raise SystemExit(1)
mn = min(pixels)
mx = max(pixels)
mean = sum(pixels) / len(pixels)
print(f"dump verify ok: {path} {width}x{height} mean={mean:.2f} min={mn} max={mx}")
if mx - mn < 8 or mean < 1.0:
    raise SystemExit(1)
PY
}

summarize_single() {
  local mode="$1"
  local rc="$2"
  local check_rc=0
  local result="PASS"
  case "${mode}" in
    model)
      print_matches model "${OUT_DIR}/model.log" 'rpi_modeld(:| synthetic| replay| .*done)'
      validate_component_log model "${OUT_DIR}/model.log" 'rpi_modeld synthetic done frames=' "${SMOKE_MIN_MODEL_FPS:-1}" || check_rc=1
      ;;
    profile)
      print_matches profile "${OUT_DIR}/model.log" 'rpi_modeld (profile|synthetic done)'
      emit_profile_metric "${OUT_DIR}/model.log" || check_rc=1
      validate_component_log model "${OUT_DIR}/model.log" 'rpi_modeld synthetic done frames=' "${SMOKE_MIN_MODEL_FPS:-1}" || check_rc=1
      ;;
    artifacts)
      print_matches artifacts "${OUT_DIR}/artifacts.log" 'ARTIFACT'
      if ! grep -q 'ARTIFACT_CHECK result=PASS' "${OUT_DIR}/artifacts.log"; then
        echo "SMOKE_CHECK component=artifacts result=FAIL"
        check_rc=1
      else
        echo "SMOKE_CHECK component=artifacts result=PASS"
      fi
      ;;
    parity)
      print_matches parity "${OUT_DIR}/parity.log" 'PREPROCESS_PARITY|IPC_ABI|NCNN_OUTPUT_CONTRACT|verify_calibration_equivalence'
      if ! grep -q 'PREPROCESS_PARITY result=PASS' "${OUT_DIR}/parity.log"; then
        echo "SMOKE_CHECK component=preprocess_parity result=FAIL"
        check_rc=1
      else
        echo "SMOKE_CHECK component=preprocess_parity result=PASS"
      fi
      if ! grep -q 'IPC_ABI result=PASS' "${OUT_DIR}/parity.log"; then
        echo "SMOKE_CHECK component=ipc_abi result=FAIL"
        check_rc=1
      else
        echo "SMOKE_CHECK component=ipc_abi result=PASS"
      fi
      if ! grep -q 'NCNN_OUTPUT_CONTRACT result=PASS' "${OUT_DIR}/parity.log"; then
        echo "SMOKE_CHECK component=ncnn_output_contract result=FAIL"
        check_rc=1
      else
        echo "SMOKE_CHECK component=ncnn_output_contract result=PASS"
      fi
      if ! grep -q 'verify_calibration_equivalence: PASS' "${OUT_DIR}/parity.log"; then
        echo "SMOKE_CHECK component=calibration_equivalence result=FAIL"
        check_rc=1
      else
        echo "SMOKE_CHECK component=calibration_equivalence result=PASS"
      fi
      ;;
    camera)
      print_matches camera "${OUT_DIR}/camera.log" 'rpi_camerad (fps|done)'
      validate_component_log camera "${OUT_DIR}/camera.log" 'rpi_camerad done frames=' "${SMOKE_MIN_CAMERA_FPS:-20}" || check_rc=1
      ;;
    camera-file|camera-replay|camera-real)
      print_matches camera "${OUT_DIR}/camera.log" 'CAMERA_AUTO_SOURCE|rpi_camerad (fps|done|error)'
      validate_component_log camera "${OUT_DIR}/camera.log" 'rpi_camerad done frames=' "${SMOKE_MIN_CAMERA_FPS:-20}" || check_rc=1
      ;;
    camera-probe)
      print_matches probe "${OUT_DIR}/probe.log" 'CAMERA_PROBE|/dev/video|No cameras|UVC|Web Camera|Camera|error'
      ;;
    manager)
      print_matches manager "${OUT_DIR}/manager.log" 'rpi_(manager|camerad|modeld|overlay).*'
      validate_manager_log || check_rc=1
      ;;
    perf)
      print_matches perf "${OUT_DIR}/perf.log" 'PERF|throttled|temp=|scaling_governor'
      ;;
  esac
  if [[ "${rc}" -ne 0 || "${check_rc}" -ne 0 ]]; then
    result="FAIL"
  fi
  echo "SMOKE result=${result} mode=${mode} rc=${rc} check_rc=${check_rc} logs=${OUT_DIR}"
  return "${check_rc}"
}

cleanup_shm() {
  rm -f /dev/shm/k230_road_ai \
        /dev/shm/k230_road_ai_frame \
        /dev/shm/k230_model_state \
        /dev/shm/k230_manager_state
}

kill_children() {
  if [[ -n "${cam_pid:-}" ]]; then kill "${cam_pid}" 2>/dev/null || true; fi
  if [[ -n "${model_pid:-}" ]]; then kill "${model_pid}" 2>/dev/null || true; fi
  if [[ -n "${overlay_pid:-}" ]]; then kill "${overlay_pid}" 2>/dev/null || true; fi
}

require_file() {
  local path="$1"
  if [[ ! -f "${path}" ]]; then
    echo "missing file: ${path}" >&2
    exit 2
  fi
}

require_executable() {
  local name="$1"
  local path="${ROOT_DIR}/${name}"
  if [[ ! -x "${path}" ]]; then
    echo "missing executable: ${path} (ROOT_DIR=${ROOT_DIR}, set RPI_RUNTIME_DIR=/path/to/runtime if needed)" >&2
    exit 2
  fi
}

file_size() {
  local path="$1"
  if stat -c '%s' "${path}" >/dev/null 2>&1; then
    stat -c '%s' "${path}"
  else
    stat -f '%z' "${path}"
  fi
}

hash_file() {
  local path="$1"
  if command -v sha256sum >/dev/null; then
    sha256sum "${path}" | awk '{print $1}'
  elif command -v shasum >/dev/null; then
    shasum -a 256 "${path}" | awk '{print $1}'
  else
    echo "NA"
    return 1
  fi
}

report_artifact_file() {
  local kind="$1"
  local path="$2"
  if [[ ! -f "${path}" ]]; then
    echo "ARTIFACT kind=${kind} result=missing path=${path}"
    return 1
  fi
  local bytes sha
  bytes="$(file_size "${path}")"
  sha="$(hash_file "${path}")"
  echo "ARTIFACT kind=${kind} result=ok bytes=${bytes} sha256=${sha} path=${path}"
}

run_artifact_report() {
  reset_outputs
  local rc=0
  {
    echo "ARTIFACT_ROOT root=${ROOT_DIR}"
    report_artifact_file model_param "${MODEL_PARAM}" || rc=1
    report_artifact_file model_bin "${MODEL_BIN}" || rc=1
    report_artifact_file ncnn_lib "${NCNN_LIBRARY}" || rc=1
    report_artifact_file binary_rpi_modeld "${ROOT_DIR}/rpi_modeld" || rc=1
    report_artifact_file binary_rpi_camerad "${ROOT_DIR}/rpi_camerad" || rc=1
    report_artifact_file binary_rpi_overlay "${ROOT_DIR}/rpi_overlay" || rc=1
    if command -v pkg-config >/dev/null && pkg-config --exists opencv4; then
      echo "ARTIFACT_TOOL kind=opencv4 version=$(pkg-config --modversion opencv4)"
    else
      echo "ARTIFACT_TOOL kind=opencv4 version=missing"
    fi
    if [[ "${rc}" -eq 0 ]]; then
      echo "ARTIFACT_CHECK result=PASS"
    else
      echo "ARTIFACT_CHECK result=FAIL"
    fi
  } > >(tee "${OUT_DIR}/artifacts.log") 2>&1
  summarize_single artifacts "${rc}"
}

probe_v4l2_nodes() {
  local candidates=0
  local dev info formats driver card bus capture external candidate reason
  shopt -s nullglob
  for dev in /dev/video*; do
    info="$(v4l2-ctl -d "${dev}" -D 2>/dev/null || true)"
    if [[ -z "${info}" ]]; then
      echo "CAMERA_PROBE_NODE device=${dev} candidate=0 reason=v4l2_query_failed"
      continue
    fi
    driver="$(sed -n 's/^[[:space:]]*Driver name[[:space:]]*:[[:space:]]*//p' <<<"${info}" | head -n 1)"
    card="$(sed -n 's/^[[:space:]]*Card type[[:space:]]*:[[:space:]]*//p' <<<"${info}" | head -n 1)"
    bus="$(sed -n 's/^[[:space:]]*Bus info[[:space:]]*:[[:space:]]*//p' <<<"${info}" | head -n 1)"
    formats="$(v4l2-ctl -d "${dev}" --list-formats-ext 2>/dev/null |
      sed -n "s/^[[:space:]]*\\[[0-9]\\+\\]: '\\([^']*\\)'.*/\\1/p" |
      paste -sd, - | cut -c1-120)"

    capture=0
    external=0
    candidate=0
    reason="not_capture_source"
    if grep -Eq '(^|[[:space:]])Video Capture($|[[:space:]])' <<<"${info}" &&
        ! grep -Eq 'Metadata Capture|Memory-to-Memory' <<<"${info}"; then
      capture=1
    fi
    if [[ "${driver}" == "uvcvideo" || "${bus}" == usb-* ]]; then
      external=1
    fi
    if [[ "${capture}" -eq 1 && "${external}" -eq 1 ]]; then
      candidate=1
      reason="external_capture"
      candidates=$((candidates + 1))
    elif [[ "${capture}" -eq 1 ]]; then
      reason="platform_or_helper_capture"
    fi

    driver="${driver// /_}"
    card="${card// /_}"
    bus="${bus// /_}"
    formats="${formats// /_}"

    echo "CAMERA_PROBE_NODE device=${dev} candidate=${candidate} capture=${capture} external=${external} driver=${driver:-unknown} card=${card:-unknown} bus=${bus:-unknown} formats=${formats:-none} reason=${reason}"
  done
  shopt -u nullglob
  echo "CAMERA_PROBE_V4L2 candidates=${candidates}"
  return 0
}

first_probe_candidate() {
  local path="$1"
  [[ -f "${path}" ]] || return 1
  awk '
    /^CAMERA_PROBE_NODE / && /candidate=1/ {
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^device=/) {
          sub(/^device=/, "", $i)
          print $i
          exit
        }
      }
    }
  ' "${path}"
}

select_camera_source() {
  if [[ -n "${RPI_CAMERA_SOURCE:-}" ]]; then
    printf '%s\n' "${RPI_CAMERA_SOURCE}"
    return 0
  fi

  : >"${OUT_DIR}/probe.log"
  if ! command -v v4l2-ctl >/dev/null; then
    echo "CAMERA_AUTO_SOURCE result=FAIL reason=v4l2_ctl_missing" >>"${OUT_DIR}/probe.log"
    return 1
  fi

  probe_v4l2_nodes >>"${OUT_DIR}/probe.log"
  local source
  source="$(first_probe_candidate "${OUT_DIR}/probe.log")"
  if [[ -z "${source}" ]]; then
    echo "CAMERA_AUTO_SOURCE result=FAIL reason=no_candidate log=${OUT_DIR}/probe.log" >>"${OUT_DIR}/probe.log"
    return 1
  fi

  echo "CAMERA_AUTO_SOURCE result=PASS source=${source}" >>"${OUT_DIR}/probe.log"
  printf '%s\n' "${source}"
}

run_camera_probe() {
  reset_outputs
  set +e
  {
    echo "=== lsusb ==="
    lsusb 2>/dev/null || true
    echo "=== /dev/video ==="
    ls -l /dev/video* 2>/dev/null || true
    echo "=== v4l2 devices ==="
    if command -v v4l2-ctl >/dev/null; then
      v4l2-ctl --list-devices 2>&1
      echo "=== v4l2 capture candidates ==="
      probe_v4l2_nodes
    else
      echo "v4l2-ctl missing"
    fi
    echo "=== rpicam cameras ==="
    if command -v rpicam-vid >/dev/null; then
      rpicam-vid --list-cameras 2>&1
    else
      echo "rpicam-vid missing"
    fi
    echo "=== recent camera usb/kernel messages ==="
    dmesg 2>/dev/null | grep -Ei 'uvc|web camera|camera|usb 1-1|device descriptor|not accepting|enumerate' | tail -n 40 || true
  } | tee "${OUT_DIR}/probe.log"

  local has_usb_camera=1
  local has_csi_camera=1
  local v4l2_candidates=0
  if lsusb 2>/dev/null | grep -Eiq 'camera|webcam|web camera|uvc|video'; then
    has_usb_camera=0
  fi
  if command -v v4l2-ctl >/dev/null; then
    v4l2_candidates="$(grep -Ec '^CAMERA_PROBE_NODE .*candidate=1' "${OUT_DIR}/probe.log" 2>/dev/null || true)"
  fi
  if command -v rpicam-vid >/dev/null &&
      ! rpicam-vid --list-cameras 2>&1 | grep -Fq 'No cameras available'; then
    has_csi_camera=0
  fi
  local rc=1
  if [[ "${has_usb_camera}" -eq 0 || "${has_csi_camera}" -eq 0 || "${v4l2_candidates}" -gt 0 ]]; then
    rc=0
  fi
  if [[ "${rc}" -eq 0 ]]; then
    echo "CAMERA_PROBE result=PASS usb_candidate=$((1 - has_usb_camera)) csi_candidate=$((1 - has_csi_camera)) v4l2_candidates=${v4l2_candidates}"
  else
    echo "CAMERA_PROBE result=FAIL usb_candidate=0 csi_candidate=0 v4l2_candidates=${v4l2_candidates}"
  fi | tee -a "${OUT_DIR}/probe.log"
  set -e
  summarize_single camera-probe "${rc}"
  return "${rc}"
}

can_make_camera_file_fixture() {
  command -v ffmpeg >/dev/null && [[ -f "${REPLAY_NV12}" ]]
}

make_camera_file_fixture() {
  require_file "${REPLAY_NV12}"
  if ! command -v ffmpeg >/dev/null; then
    echo "ffmpeg missing; cannot generate camera file fixture" >&2
    return 2
  fi
  local path="${OUT_DIR}/camera_source.avi"
  local frames="${CAMERA_FILE_FRAMES:-60}"
  rm -f "${path}"
  ffmpeg -hide_banner -loglevel error -y \
    -f rawvideo -pix_fmt nv12 -s:v 512x256 -r "${CAMERA_FPS}" \
    -skip_initial_bytes 20 -i "${REPLAY_NV12}" \
    -frames:v "${frames}" -vf scale=1024:512 -c:v mjpeg -q:v 4 "${path}"
  if [[ ! -s "${path}" ]]; then
    echo "failed to create camera file fixture: ${path}" >&2
    return 1
  fi
  echo "${path}"
}

run_model_only() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  require_executable rpi_modeld
  reset_outputs
  set +e
  SUPERCOMBO_MAX_FRAMES="${MODEL_FRAMES:-60}" \
    RPI_SYNTHETIC=1 \
    RPI_SYNTHETIC_FRAMES="${MODEL_FRAMES:-60}" \
    "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" 2>&1 | tee "${OUT_DIR}/model.log"
  local rc=${PIPESTATUS[0]}
  set -e
  local summary_rc=0
  summarize_single model "${rc}" || summary_rc=$?
  if [[ "${rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_profile_snapshot() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  require_executable rpi_modeld
  reset_outputs
  local frames="${PROFILE_MODEL_FRAMES:-40}"
  set +e
  SUPERCOMBO_MAX_FRAMES="${frames}" \
    RPI_SYNTHETIC=1 \
    RPI_SYNTHETIC_FRAMES="${frames}" \
    RPI_PROFILE_MODEL=1 \
    "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" 2>&1 | tee "${OUT_DIR}/model.log"
  local rc=${PIPESTATUS[0]}
  set -e
  local summary_rc=0
  summarize_single profile "${rc}" || summary_rc=$?
  if [[ "${rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_parity_checks() {
  reset_outputs
  require_executable check_preprocess_parity
  require_executable check_ipc_abi
  require_executable check_ncnn_output_contract
  require_executable verify_calibration_equivalence
  local rc=0
  {
    "${ROOT_DIR}/check_preprocess_parity"
    "${ROOT_DIR}/check_ipc_abi"
    "${ROOT_DIR}/check_ncnn_output_contract"
    "${ROOT_DIR}/verify_calibration_equivalence"
  } 2>&1 | tee "${OUT_DIR}/parity.log" || rc=$?
  summarize_single parity "${rc}"
}

run_camera_only() {
  local source_mode="$1"
  local summary_mode="camera"
  if [[ "${source_mode}" == "replay" ]]; then
    summary_mode="camera-replay"
  elif [[ "${source_mode}" == "file" ]]; then
    summary_mode="camera-file"
  elif [[ "${source_mode}" == "real" ]]; then
    summary_mode="camera-real"
  fi
  reset_outputs
  cleanup_shm
  require_executable rpi_camerad
  set +e
  if [[ "${source_mode}" == "replay" ]]; then
    require_file "${REPLAY_NV12}"
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}" \
      RPI_CAMERA_REPLAY_NV12="${REPLAY_NV12}" \
      RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" 2>&1 | tee "${OUT_DIR}/camera.log"
  elif [[ "${source_mode}" == "file" ]]; then
    local camera_file
    camera_file="$(make_camera_file_fixture)"
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}" \
      RPI_CAMERA_SOURCE="${camera_file}" \
      RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" 2>&1 | tee "${OUT_DIR}/camera.log"
  elif [[ "${source_mode}" == "real" ]]; then
    local real_source
    real_source="$(select_camera_source || true)"
    if [[ -z "${real_source}" ]]; then
      {
        echo "CAMERA_AUTO_SOURCE result=FAIL reason=no_candidate probe=${OUT_DIR}/probe.log"
        if [[ -f "${OUT_DIR}/probe.log" ]]; then
          grep -E 'CAMERA_AUTO_SOURCE|CAMERA_PROBE_NODE|CAMERA_PROBE_V4L2' "${OUT_DIR}/probe.log" || true
        fi
        false
      } 2>&1 | tee "${OUT_DIR}/camera.log"
    else
      {
        if [[ -n "${RPI_CAMERA_SOURCE:-}" ]]; then
          echo "CAMERA_AUTO_SOURCE result=SKIP reason=env source=${real_source}"
        else
          echo "CAMERA_AUTO_SOURCE result=PASS source=${real_source}"
        fi
        local camera_real_timeout="${CAMERA_REAL_TIMEOUT_SEC:-15}"
        local camera_real_env=(
          SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}"
          RPI_CAMERA_SOURCE="${real_source}"
          RPI_CAMERA_FPS="${CAMERA_FPS}"
        )
        if command -v timeout >/dev/null && [[ "${camera_real_timeout}" != "0" ]]; then
          timeout "${camera_real_timeout}s" env "${camera_real_env[@]}" "${ROOT_DIR}/rpi_camerad"
        else
          env "${camera_real_env[@]}" "${ROOT_DIR}/rpi_camerad"
        fi
      } 2>&1 | tee "${OUT_DIR}/camera.log"
    fi
  else
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}" \
      RPI_CAMERA_SYNTHETIC=1 \
      RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" 2>&1 | tee "${OUT_DIR}/camera.log"
  fi
  local rc=${PIPESTATUS[0]}
  set -e
  local summary_rc=0
  summarize_single "${summary_mode}" "${rc}" || summary_rc=$?
  if [[ "${rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_pipeline() {
  local source_mode="$1"
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  require_executable rpi_camerad
  require_executable rpi_modeld
  require_executable rpi_overlay
  reset_outputs
  cleanup_shm
  trap 'kill_children' EXIT

  if [[ "${source_mode}" == "replay" ]]; then
    require_file "${REPLAY_NV12}"
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-240}" \
    RPI_CAMERA_REPLAY_NV12="${REPLAY_NV12}" \
    RPI_CAMERA_REPLAY_LOOP=1 \
    RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" >"${OUT_DIR}/camera.log" 2>&1 &
  else
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-240}" \
    RPI_CAMERA_SYNTHETIC=1 \
    RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" >"${OUT_DIR}/camera.log" 2>&1 &
  fi
  cam_pid=$!

  sleep 0.5
  local model_timeout="${MODEL_TIMEOUT_SEC:-60}"
  local model_env=(SUPERCOMBO_MAX_FRAMES="${MODEL_FRAMES:-80}")
  if command -v timeout >/dev/null && [[ "${model_timeout}" != "0" ]]; then
    timeout "${model_timeout}s" env "${model_env[@]}" \
      "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" >"${OUT_DIR}/model.log" 2>&1 &
  else
    env "${model_env[@]}" \
      "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" >"${OUT_DIR}/model.log" 2>&1 &
  fi
  model_pid=$!

  sleep 0.2
  overlay_dump="${OVERLAY_DUMP:-}"
  if [[ "${DUMP:-0}" != "0" && -z "${overlay_dump}" ]]; then
    overlay_dump="${OUT_DIR}/overlay.ppm"
  fi
  if [[ -n "${overlay_dump}" ]]; then
    rm -f "${overlay_dump}"
  fi
  overlay_env=(
    SUPERCOMBO_MAX_FRAMES="${OVERLAY_FRAMES:-40}"
    RPI_DISPLAY="${DISPLAY_MODE}"
    RPI_OVERLAY_FPS="${OVERLAY_FPS}"
  )
  if [[ -n "${overlay_dump}" ]]; then
    overlay_env+=(RPI_OVERLAY_DUMP="${overlay_dump}")
  fi
  overlay_timeout="${OVERLAY_TIMEOUT_SEC:-20}"
  set +e
  if command -v timeout >/dev/null; then
    timeout "${overlay_timeout}s" env "${overlay_env[@]}" "${ROOT_DIR}/rpi_overlay" >"${OUT_DIR}/overlay.log" 2>&1
  else
    env "${overlay_env[@]}" "${ROOT_DIR}/rpi_overlay" >"${OUT_DIR}/overlay.log" 2>&1
  fi
  overlay_rc=$?

  wait "${model_pid}"
  model_rc=$?
  dump_rc=0
  if [[ -n "${overlay_dump}" ]]; then
    verify_ppm_dump "${overlay_dump}" >"${OUT_DIR}/dump_verify.log" 2>&1
    dump_rc=$?
  fi
  set -e
  kill "${cam_pid}" 2>/dev/null || true
  wait "${cam_pid}" 2>/dev/null || true
  trap - EXIT

  if [[ -f "${OUT_DIR}/dump_verify.log" ]]; then
    cat "${OUT_DIR}/dump_verify.log"
  fi
  local summary_rc=0
  summarize_pipeline "${source_mode}" "${model_rc}" "${overlay_rc}" "${overlay_dump}" "${dump_rc}" || summary_rc=$?
  if [[ "${model_rc}" -ne 0 || "${overlay_rc}" -ne 0 || "${dump_rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_manager() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  require_executable rpi_manager.py
  require_executable rpi_camerad
  require_executable rpi_modeld
  if [[ "${RPI_RUN_OVERLAY:-1}" != "0" ]]; then
    require_executable rpi_overlay
  fi
  reset_outputs
  set +e
  RPI_CAMERA_SYNTHETIC="${RPI_CAMERA_SYNTHETIC:-1}" \
    RPI_DISPLAY="${DISPLAY_MODE}" \
    RPI_MANAGER_MAX_SEC="${RPI_MANAGER_MAX_SEC:-10}" \
    RPI_OVERLAY_FPS="${OVERLAY_FPS}" \
    "${ROOT_DIR}/rpi_manager.py" "${MODEL_PARAM}" "${MODEL_BIN}" 2>&1 | tee "${OUT_DIR}/manager.log"
  local rc=${PIPESTATUS[0]}
  set -e
  local summary_rc=0
  summarize_single manager "${rc}" || summary_rc=$?
  if [[ "${rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

append_system_perf_state() {
  {
    echo "=== cpu ==="
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      [[ -f "${governor}" ]] && echo "${governor}=$(cat "${governor}")"
    done
    for freq in /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq; do
      [[ -f "${freq}" ]] && echo "${freq}=$(cat "${freq}")"
    done
    if command -v vcgencmd >/dev/null; then
      vcgencmd get_throttled || true
      vcgencmd measure_temp || true
      vcgencmd measure_clock arm || true
    fi
  } >>"${OUT_DIR}/perf.log"
}

fps_ge() {
  local actual="$1"
  local minimum="$2"
  [[ -n "${actual}" && "${actual}" != "NA" ]] || return 1
  awk -v actual="${actual}" -v minimum="${minimum}" \
    'BEGIN { exit !((actual + 0.0) >= (minimum + 0.0)) }'
}

record_perf_check() {
  local kind="$1"
  local name="$2"
  local metric="$3"
  local actual="$4"
  local minimum="$5"
  if fps_ge "${actual}" "${minimum}"; then
    echo "PERF_CHECK kind=${kind} name=${name} metric=${metric} actual=${actual} min=${minimum} result=PASS" | tee -a "${OUT_DIR}/perf.log"
    return 0
  fi
  echo "PERF_CHECK kind=${kind} name=${name} metric=${metric} actual=${actual:-NA} min=${minimum} result=FAIL" | tee -a "${OUT_DIR}/perf.log"
  return 1
}

run_perf_model_case() {
  local name="$1"
  local min_fps="$2"
  shift 2
  local frames="${PERF_MODEL_FRAMES:-60}"
  local attempts="${PERF_ATTEMPTS:-2}"
  local attempt=1
  while [[ "${attempt}" -le "${attempts}" ]]; do
    echo "=== model ${name} attempt=${attempt} ===" >>"${OUT_DIR}/perf.log"
    set +e
    env "$@" \
      SUPERCOMBO_MAX_FRAMES="${frames}" \
      RPI_SYNTHETIC=1 \
      RPI_SYNTHETIC_FRAMES="${frames}" \
      "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" \
      >"${OUT_DIR}/perf_model_${name}_attempt${attempt}.log" 2>&1
    local rc=$?
    set -e
    tr '\r' '\n' <"${OUT_DIR}/perf_model_${name}_attempt${attempt}.log" | tail -n 12 >>"${OUT_DIR}/perf.log"
    local fps
    fps="$(tr '\r' '\n' <"${OUT_DIR}/perf_model_${name}_attempt${attempt}.log" |
      sed -n 's/.*rpi_modeld synthetic done frames=.* fps=\([0-9.]*\).*/\1/p' | tail -n 1)"
    echo "PERF model name=${name} attempt=${attempt} rc=${rc} fps=${fps:-NA}" | tee -a "${OUT_DIR}/perf.log"
    if [[ "${rc}" -eq 0 ]] && record_perf_check model "${name}" fps "${fps:-NA}" "${min_fps}"; then
      return 0
    fi
    if [[ "${attempt}" -lt "${attempts}" ]]; then
      echo "PERF_RETRY kind=model name=${name} next_attempt=$((attempt + 1))" | tee -a "${OUT_DIR}/perf.log"
    fi
    attempt=$((attempt + 1))
  done
  return 1
}

run_perf_manager_case() {
  local name="$1"
  local min_camera_fps="$2"
  local min_model_fps="$3"
  shift 3
  local seconds="${PERF_MANAGER_SEC:-5}"
  local attempts="${PERF_ATTEMPTS:-2}"
  local attempt=1
  while [[ "${attempt}" -le "${attempts}" ]]; do
    echo "=== manager ${name} attempt=${attempt} ===" >>"${OUT_DIR}/perf.log"
    set +e
    env "$@" \
      RPI_CAMERA_SYNTHETIC="${RPI_CAMERA_SYNTHETIC:-1}" \
      RPI_MANAGER_MAX_SEC="${seconds}" \
      "${ROOT_DIR}/rpi_manager.py" "${MODEL_PARAM}" "${MODEL_BIN}" \
      >"${OUT_DIR}/perf_manager_${name}_attempt${attempt}.log" 2>&1
    local rc=$?
    set -e
    tr '\r' '\n' <"${OUT_DIR}/perf_manager_${name}_attempt${attempt}.log" | tail -n 16 >>"${OUT_DIR}/perf.log"
    local camera_fps model_fps overlay_frames
    camera_fps="$(tr '\r' '\n' <"${OUT_DIR}/perf_manager_${name}_attempt${attempt}.log" |
      sed -n 's/.*rpi_camerad done frames=.* fps=\([0-9.]*\).*/\1/p' | tail -n 1)"
    model_fps="$(tr '\r' '\n' <"${OUT_DIR}/perf_manager_${name}_attempt${attempt}.log" |
      sed -n 's/.*rpi_modeld done frames=.* fps=\([0-9.]*\).*/\1/p' | tail -n 1)"
    overlay_frames="$(tr '\r' '\n' <"${OUT_DIR}/perf_manager_${name}_attempt${attempt}.log" |
      sed -n 's/.*rpi_overlay done frames=\([0-9]*\).*/\1/p' | tail -n 1)"
    echo "PERF manager name=${name} attempt=${attempt} rc=${rc} camera_fps=${camera_fps:-NA} model_fps=${model_fps:-NA} overlay_frames=${overlay_frames:-NA}" | tee -a "${OUT_DIR}/perf.log"
    local check_rc=0
    if [[ "${rc}" -eq 0 ]]; then
      record_perf_check manager "${name}" camera_fps "${camera_fps:-NA}" "${min_camera_fps}" || check_rc=1
      record_perf_check manager "${name}" model_fps "${model_fps:-NA}" "${min_model_fps}" || check_rc=1
      if [[ "${check_rc}" -eq 0 ]]; then
        return 0
      fi
    fi
    if [[ "${attempt}" -lt "${attempts}" ]]; then
      echo "PERF_RETRY kind=manager name=${name} next_attempt=$((attempt + 1))" | tee -a "${OUT_DIR}/perf.log"
    fi
    attempt=$((attempt + 1))
  done
  return 1
}

run_perf_snapshot() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  reset_outputs
  cleanup_shm
  append_system_perf_state

  local rc=0
  run_perf_model_case default "${PERF_MIN_MODEL_FPS:-8}" || rc=1
  run_perf_model_case threads3 "${PERF_MIN_THREADS3_FPS:-12}" RPI_NCNN_THREADS=3 || rc=1
  run_perf_model_case input_bf16 "${PERF_MIN_INPUT_BF16_FPS:-15}" RPI_NCNN_INPUT_BF16=1 || rc=1
  run_perf_manager_case no_overlay "${PERF_MIN_MANAGER_CAMERA_FPS:-25}" "${PERF_MIN_MANAGER_MODEL_FPS:-12}" RPI_RUN_OVERLAY=0 || rc=1
  run_perf_manager_case overlay_headless_2fps "${PERF_MIN_MANAGER_CAMERA_FPS:-25}" "${PERF_MIN_MANAGER_MODEL_FPS:-12}" RPI_RUN_OVERLAY=1 RPI_DISPLAY=0 RPI_OVERLAY_FPS=2 || rc=1
  run_perf_manager_case overlay_fb_2fps "${PERF_MIN_MANAGER_CAMERA_FPS:-25}" "${PERF_MIN_MANAGER_FB_MODEL_FPS:-8}" RPI_RUN_OVERLAY=1 RPI_DISPLAY=fb RPI_OVERLAY_FPS=2 || rc=1

  local summary_rc=0
  summarize_single perf "${rc}" || summary_rc=$?
  if [[ "${rc}" -ne 0 || "${summary_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_check_step() {
  local name="$1"
  shift
  local step_dir="${CHECK_OUT_DIR}/${name}"
  mkdir -p "${step_dir}"
  echo "CHECK_STEP name=${name} status=running logs=${step_dir}" | tee -a "${CHECK_OUT_DIR}/check.log"
  set +e
  (
    OUT_DIR="${step_dir}"
    DISPLAY_MODE=0
    OVERLAY_FPS="${CHECK_OVERLAY_FPS:-2}"
    RPI_DISPLAY=0
    RPI_RUN_OVERLAY=1
    RPI_CLEAR_SHM=1
    RPI_NO_RESTART=0
    export RPI_DISPLAY RPI_RUN_OVERLAY RPI_CLEAR_SHM RPI_NO_RESTART
    unset RPI_CAMERA_SOURCE
    unset RPI_CAMERA_REPLAY_NV12
    unset SUPERCOMBO_REPLAY_NV12
    unset RPI_DISPLAY_FB
    "$@"
  )
  local rc=$?
  set -e
  local result="PASS"
  if [[ "${rc}" -ne 0 ]]; then
    result="FAIL"
  fi
  append_check_step_summary "${name}" "${step_dir}"
  echo "CHECK_STEP name=${name} rc=${rc} result=${result} logs=${step_dir}" | tee -a "${CHECK_OUT_DIR}/check.log"
  return "${rc}"
}

check_metric_from_log() {
  local step="$1"
  local component="$2"
  local path="$3"
  local pattern="$4"

  local line
  if ! line="$(last_log_line "${path}" "${pattern}")"; then
    echo "CHECK_METRIC step=${step} component=${component} result=missing log=${path}"
    return 0
  fi

  local frames errors fps missed model_seq have_model
  frames="$(field_from_line "${line}" frames)"
  errors="$(field_from_line "${line}" errors)"
  fps="$(field_from_line "${line}" fps)"
  missed="$(field_from_line "${line}" missed)"
  model_seq="$(field_from_line "${line}" model_seq)"
  have_model="$(field_from_line "${line}" have_model)"
  echo "CHECK_METRIC step=${step} component=${component} frames=${frames:-NA} missed=${missed:-NA} errors=${errors:-NA} fps=${fps:-NA} model_seq=${model_seq:-NA} have_model=${have_model:-NA}"
}

append_check_step_summary() {
  local name="$1"
  local step_dir="$2"
  {
    case "${name}" in
      artifacts)
        if [[ -f "${step_dir}/artifacts.log" ]]; then
          tr '\r' '\n' <"${step_dir}/artifacts.log" |
            grep -E '^ARTIFACT' |
            sed "s/^/CHECK_DETAIL step=${name} /" || true
        fi
        ;;
      model)
        check_metric_from_log "${name}" model "${step_dir}/model.log" 'rpi_modeld synthetic done frames='
        ;;
      profile)
        if [[ -f "${step_dir}/model.log" ]]; then
          emit_profile_metric "${step_dir}/model.log" |
            sed "s/^/CHECK_DETAIL step=${name} /" || true
        fi
        check_metric_from_log "${name}" model "${step_dir}/model.log" 'rpi_modeld synthetic done frames='
        ;;
      parity)
        if [[ -f "${step_dir}/parity.log" ]]; then
          tr '\r' '\n' <"${step_dir}/parity.log" |
            grep -E '^(PREPROCESS_PARITY|IPC_ABI|NCNN_OUTPUT_CONTRACT|verify_calibration_equivalence)' |
            sed "s/^/CHECK_DETAIL step=${name} /" || true
        fi
        ;;
      camera|camera_file|camera_replay|camera_real)
        check_metric_from_log "${name}" camera "${step_dir}/camera.log" 'rpi_camerad done frames='
        ;;
      synthetic|replay)
        check_metric_from_log "${name}" camera "${step_dir}/camera.log" 'rpi_camerad done frames='
        check_metric_from_log "${name}" model "${step_dir}/model.log" 'rpi_modeld (synthetic )?done frames='
        check_metric_from_log "${name}" overlay "${step_dir}/overlay.log" 'rpi_overlay done frames='
        ;;
      manager)
        check_metric_from_log "${name}" camera "${step_dir}/manager.log" 'rpi_camerad done frames='
        check_metric_from_log "${name}" model "${step_dir}/manager.log" 'rpi_modeld done frames='
        check_metric_from_log "${name}" overlay "${step_dir}/manager.log" 'rpi_overlay done frames='
        ;;
      perf)
        if [[ -f "${step_dir}/perf.log" ]]; then
          tr '\r' '\n' <"${step_dir}/perf.log" |
            grep -E '^PERF(_CHECK)? ' |
            sed "s/^/CHECK_DETAIL step=${name} /" || true
        fi
        ;;
      camera_probe)
        if [[ -f "${step_dir}/probe.log" ]]; then
          tr '\r' '\n' <"${step_dir}/probe.log" |
            grep -E '^CAMERA_PROBE(_V4L2)? |^CAMERA_PROBE_NODE .*candidate=1' |
            sed "s/^/CHECK_DETAIL step=${name} /" || true
        fi
        ;;
    esac
  } | tee -a "${CHECK_OUT_DIR}/check.log"
}

run_check_skip() {
  local name="$1"
  local reason="$2"
  echo "CHECK_STEP name=${name} rc=0 result=SKIP reason=${reason}" | tee -a "${CHECK_OUT_DIR}/check.log"
}

run_check_model() {
  MODEL_FRAMES="${CHECK_MODEL_FRAMES:-20}"
  run_model_only
}

run_check_artifacts() {
  run_artifact_report
}

run_check_parity() {
  run_parity_checks
}

run_check_profile() {
  PROFILE_MODEL_FRAMES="${CHECK_PROFILE_MODEL_FRAMES:-30}"
  run_profile_snapshot
}

run_check_camera() {
  CAMERA_FRAMES="${CHECK_CAMERA_FRAMES:-30}"
  run_camera_only synthetic
}

run_check_camera_replay() {
  CAMERA_FRAMES="${CHECK_CAMERA_REPLAY_FRAMES:-30}"
  run_camera_only replay
}

run_check_camera_file() {
  CAMERA_FRAMES="${CHECK_CAMERA_FILE_FRAMES:-30}"
  CAMERA_FILE_FRAMES="${CHECK_CAMERA_FILE_SOURCE_FRAMES:-60}"
  run_camera_only file
}

run_check_synthetic() {
  CAMERA_FRAMES="${CHECK_SYNTHETIC_CAMERA_FRAMES:-120}"
  MODEL_FRAMES="${CHECK_SYNTHETIC_MODEL_FRAMES:-12}"
  OVERLAY_FRAMES="${CHECK_SYNTHETIC_OVERLAY_FRAMES:-5}"
  OVERLAY_TIMEOUT_SEC="${CHECK_OVERLAY_TIMEOUT_SEC:-15}"
  MODEL_TIMEOUT_SEC="${CHECK_MODEL_TIMEOUT_SEC:-30}"
  DUMP="${CHECK_SYNTHETIC_DUMP:-1}"
  run_pipeline synthetic
}

run_check_replay() {
  CAMERA_FRAMES="${CHECK_REPLAY_CAMERA_FRAMES:-120}"
  MODEL_FRAMES="${CHECK_REPLAY_MODEL_FRAMES:-12}"
  OVERLAY_FRAMES="${CHECK_REPLAY_OVERLAY_FRAMES:-5}"
  OVERLAY_TIMEOUT_SEC="${CHECK_OVERLAY_TIMEOUT_SEC:-15}"
  MODEL_TIMEOUT_SEC="${CHECK_MODEL_TIMEOUT_SEC:-30}"
  DUMP="${CHECK_REPLAY_DUMP:-1}"
  run_pipeline replay
}

run_check_manager() {
  RPI_CAMERA_SYNTHETIC=1
  RPI_MANAGER_MAX_SEC="${CHECK_MANAGER_SEC:-5}"
  run_manager
}

run_check_perf() {
  PERF_MODEL_FRAMES="${CHECK_PERF_MODEL_FRAMES:-40}"
  PERF_MANAGER_SEC="${CHECK_PERF_MANAGER_SEC:-4}"
  run_perf_snapshot
}

run_check_suite() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"

  local base="${OUT_DIR}/check"
  rm -rf "${base}"
  mkdir -p "${base}"
  CHECK_OUT_DIR="${base}"

  local rc=0
  if [[ "${CHECK_WITH_ARTIFACTS:-1}" != "0" ]]; then
    run_check_step artifacts run_check_artifacts || rc=1
  fi
  if [[ "${CHECK_WITH_PARITY:-1}" != "0" ]]; then
    run_check_step parity run_check_parity || rc=1
  fi
  run_check_step model run_check_model || rc=1
  run_check_step camera run_check_camera || rc=1
  local include_camera_file="${CHECK_INCLUDE_CAMERA_FILE:-auto}"
  if [[ "${include_camera_file}" == "1" || ( "${include_camera_file}" == "auto" && can_make_camera_file_fixture ) ]]; then
    run_check_step camera_file run_check_camera_file || rc=1
  elif [[ "${include_camera_file}" == "auto" || "${include_camera_file}" == "0" ]]; then
    run_check_skip camera_file "missing_or_disabled_fixture"
  else
    echo "invalid CHECK_INCLUDE_CAMERA_FILE=${include_camera_file}; use auto, 0, or 1" >&2
    rc=1
  fi
  run_check_step synthetic run_check_synthetic || rc=1
  run_check_step manager run_check_manager || rc=1

  if [[ "${CHECK_WITH_PROFILE:-0}" != "0" ]]; then
    run_check_step profile run_check_profile || rc=1
  fi

  local include_replay="${CHECK_INCLUDE_REPLAY:-auto}"
  if [[ "${include_replay}" == "1" || ( "${include_replay}" == "auto" && -f "${REPLAY_NV12}" ) ]]; then
    run_check_step camera_replay run_check_camera_replay || rc=1
    run_check_step replay run_check_replay || rc=1
  elif [[ "${include_replay}" == "auto" || "${include_replay}" == "0" ]]; then
    run_check_skip replay "missing_or_disabled_replay"
  else
    echo "invalid CHECK_INCLUDE_REPLAY=${include_replay}; use auto, 0, or 1" >&2
    rc=1
  fi

  if [[ "${CHECK_WITH_PERF:-0}" != "0" ]]; then
    run_check_step perf run_check_perf || rc=1
  fi

  if [[ "${CHECK_WITH_CAMERA_PROBE:-0}" != "0" ]]; then
    run_check_step camera_probe run_camera_probe || true
  fi

  local result="PASS"
  if [[ "${rc}" -ne 0 ]]; then
    result="FAIL"
  fi
  echo "SMOKE result=${result} mode=check rc=${rc} logs=${base}" | tee -a "${CHECK_OUT_DIR}/check.log"
  return "${rc}"
}

case "${MODE}" in
  artifacts)
    run_artifact_report
    ;;
  model)
    run_model_only
    ;;
  profile)
    run_profile_snapshot
    ;;
  parity)
    run_parity_checks
    ;;
  camera)
    run_camera_only synthetic
    ;;
  camera-replay)
    run_camera_only replay
    ;;
  camera-file)
    run_camera_only file
    ;;
  camera-real)
    run_camera_only real
    ;;
  camera-probe)
    run_camera_probe
    ;;
  synthetic)
    run_pipeline synthetic
    ;;
  replay)
    run_pipeline replay
    ;;
  manager)
    run_manager
    ;;
  perf)
    run_perf_snapshot
    ;;
  check)
    run_check_suite
    ;;
  *)
    echo "usage: $0 {artifacts|model|profile|parity|camera|camera-file|camera-replay|camera-real|camera-probe|synthetic|replay|manager|perf|check}" >&2
    exit 2
    ;;
esac
