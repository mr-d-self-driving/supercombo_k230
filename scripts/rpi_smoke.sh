#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-replay}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
if [[ ! -x "${ROOT_DIR}/rpi_modeld" && -x "${SCRIPT_DIR}/rpi_modeld" ]]; then
  ROOT_DIR="${SCRIPT_DIR}"
fi

MODEL_PARAM="${MODEL_PARAM:-/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param}"
MODEL_BIN="${MODEL_BIN:-/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin}"
REPLAY_NV12="${REPLAY_NV12:-/home/chan/supercombo_models/replay_120.scnv12}"
OUT_DIR="${OUT_DIR:-/tmp/rpi_smoke}"
DISPLAY_MODE="${RPI_DISPLAY:-fb}"
OVERLAY_FPS="${RPI_OVERLAY_FPS:-10}"
CAMERA_FPS="${RPI_CAMERA_FPS:-30}"

mkdir -p "${OUT_DIR}"

reset_outputs() {
  rm -f "${OUT_DIR}/camera.log" \
        "${OUT_DIR}/model.log" \
        "${OUT_DIR}/overlay.log" \
        "${OUT_DIR}/manager.log" \
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

summarize_pipeline() {
  local source_mode="$1"
  local model_rc="$2"
  local overlay_rc="$3"
  local overlay_dump="$4"
  local result="PASS"
  if [[ "${model_rc}" -ne 0 || "${overlay_rc}" -ne 0 ]]; then
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
  echo "SMOKE result=${result} mode=${source_mode} model_rc=${model_rc} overlay_rc=${overlay_rc} dump=${overlay_dump:-none} logs=${OUT_DIR}"
}

summarize_single() {
  local mode="$1"
  local rc="$2"
  local result="PASS"
  if [[ "${rc}" -ne 0 ]]; then
    result="FAIL"
  fi
  case "${mode}" in
    model)
      print_matches model "${OUT_DIR}/model.log" 'rpi_modeld(:| synthetic| replay| .*done)'
      ;;
    camera)
      print_matches camera "${OUT_DIR}/camera.log" 'rpi_camerad (fps|done)'
      ;;
    manager)
      print_matches manager "${OUT_DIR}/manager.log" 'rpi_(manager|camerad|modeld|overlay).*'
      ;;
  esac
  echo "SMOKE result=${result} mode=${mode} rc=${rc} logs=${OUT_DIR}"
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

run_model_only() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  reset_outputs
  set +e
  SUPERCOMBO_MAX_FRAMES="${MODEL_FRAMES:-60}" \
    RPI_SYNTHETIC=1 \
    RPI_SYNTHETIC_FRAMES="${MODEL_FRAMES:-60}" \
    "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" 2>&1 | tee "${OUT_DIR}/model.log"
  local rc=${PIPESTATUS[0]}
  set -e
  summarize_single model "${rc}"
  return "${rc}"
}

run_camera_only() {
  reset_outputs
  cleanup_shm
  set +e
  if [[ "${CAMERA_SOURCE_MODE:-synthetic}" == "replay" ]]; then
    require_file "${REPLAY_NV12}"
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}" \
      RPI_CAMERA_REPLAY_NV12="${REPLAY_NV12}" \
      RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" 2>&1 | tee "${OUT_DIR}/camera.log"
  else
    SUPERCOMBO_MAX_FRAMES="${CAMERA_FRAMES:-30}" \
      RPI_CAMERA_SYNTHETIC=1 \
      RPI_CAMERA_FPS="${CAMERA_FPS}" \
      "${ROOT_DIR}/rpi_camerad" 2>&1 | tee "${OUT_DIR}/camera.log"
  fi
  local rc=${PIPESTATUS[0]}
  set -e
  summarize_single camera "${rc}"
  return "${rc}"
}

run_pipeline() {
  local source_mode="$1"
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
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
  SUPERCOMBO_MAX_FRAMES="${MODEL_FRAMES:-80}" \
    "${ROOT_DIR}/rpi_modeld" "${MODEL_PARAM}" "${MODEL_BIN}" >"${OUT_DIR}/model.log" 2>&1 &
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
  set +e
  env "${overlay_env[@]}" "${ROOT_DIR}/rpi_overlay" >"${OUT_DIR}/overlay.log" 2>&1
  overlay_rc=$?

  wait "${model_pid}"
  model_rc=$?
  set -e
  kill "${cam_pid}" 2>/dev/null || true
  wait "${cam_pid}" 2>/dev/null || true
  trap - EXIT

  summarize_pipeline "${source_mode}" "${model_rc}" "${overlay_rc}" "${overlay_dump}"
  if [[ "${model_rc}" -ne 0 || "${overlay_rc}" -ne 0 ]]; then
    return 1
  fi
  return 0
}

run_manager() {
  require_file "${MODEL_PARAM}"
  require_file "${MODEL_BIN}"
  reset_outputs
  set +e
  RPI_CAMERA_SYNTHETIC="${RPI_CAMERA_SYNTHETIC:-1}" \
    RPI_DISPLAY="${DISPLAY_MODE}" \
    RPI_MANAGER_MAX_SEC="${RPI_MANAGER_MAX_SEC:-10}" \
    RPI_OVERLAY_FPS="${OVERLAY_FPS}" \
    "${ROOT_DIR}/rpi_manager.py" "${MODEL_PARAM}" "${MODEL_BIN}" 2>&1 | tee "${OUT_DIR}/manager.log"
  local rc=${PIPESTATUS[0]}
  set -e
  summarize_single manager "${rc}"
  return "${rc}"
}

case "${MODE}" in
  model)
    run_model_only
    ;;
  camera)
    run_camera_only
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
  *)
    echo "usage: $0 {model|camera|synthetic|replay|manager}" >&2
    exit 2
    ;;
esac
