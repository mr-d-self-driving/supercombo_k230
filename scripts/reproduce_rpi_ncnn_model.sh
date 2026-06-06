#!/usr/bin/env bash
set -euo pipefail

ONNX="${REPRO_ONNX:-/home/chan/onnx_bench/supercombo_no_big_drop_pruned_viz.onnx}"
ONNX2NCNN="${REPRO_ONNX2NCNN:-/home/chan/onnx_bench/ncnn-build-onnx2ncnn-repro/build/onnx/onnx2ncnn}"
NCNNOPTIMIZE="${REPRO_NCNNOPTIMIZE:-/tmp/ncnn-tools-a72/tools/ncnnoptimize}"
OUT_DIR="${REPRO_OUT_DIR:-/home/chan/onnx_bench/ncnn-repro-model}"
OPT_FLAG="${REPRO_OPT_FLAG:-0}"

EXPECTED_PARAM_SHA="${EXPECTED_PARAM_SHA:-e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2}"
EXPECTED_BIN_SHA="${EXPECTED_BIN_SHA:-88dc46956eb5255265c9695a29dc4fba7ec6e419e5af26de137df756c3ec277b}"

require_file() {
  local path="$1"
  if [[ ! -f "${path}" ]]; then
    echo "missing required file: ${path}" >&2
    exit 2
  fi
}

require_exec() {
  local path="$1"
  if [[ ! -x "${path}" ]]; then
    echo "missing executable: ${path}" >&2
    exit 2
  fi
}

sha256_file() {
  sha256sum "$1" | cut -d' ' -f1
}

require_file "${ONNX}"
require_exec "${ONNX2NCNN}"
require_exec "${NCNNOPTIMIZE}"

mkdir -p "${OUT_DIR}"

BASE_PARAM="${OUT_DIR}/supercombo_no_big_drop_pruned_viz.param"
BASE_BIN="${OUT_DIR}/supercombo_no_big_drop_pruned_viz.bin"
OPT_PARAM="${OUT_DIR}/supercombo_no_big_drop_pruned_viz_opt.param"
OPT_BIN="${OUT_DIR}/supercombo_no_big_drop_pruned_viz_opt.bin"

"${ONNX2NCNN}" "${ONNX}" "${BASE_PARAM}" "${BASE_BIN}"
"${NCNNOPTIMIZE}" "${BASE_PARAM}" "${BASE_BIN}" "${OPT_PARAM}" "${OPT_BIN}" "${OPT_FLAG}"

PARAM_SHA="$(sha256_file "${OPT_PARAM}")"
BIN_SHA="$(sha256_file "${OPT_BIN}")"

echo "REPRO_MODEL onnx=${ONNX}"
echo "REPRO_MODEL onnx2ncnn=${ONNX2NCNN}"
echo "REPRO_MODEL ncnnoptimize=${NCNNOPTIMIZE}"
echo "REPRO_MODEL opt_flag=${OPT_FLAG}"
echo "REPRO_MODEL param_sha=${PARAM_SHA} path=${OPT_PARAM}"
echo "REPRO_MODEL bin_sha=${BIN_SHA} path=${OPT_BIN}"

if [[ "${PARAM_SHA}" == "${EXPECTED_PARAM_SHA}" && "${BIN_SHA}" == "${EXPECTED_BIN_SHA}" ]]; then
  echo "REPRO_MODEL result=PASS"
else
  echo "REPRO_MODEL result=FAIL expected_param=${EXPECTED_PARAM_SHA} expected_bin=${EXPECTED_BIN_SHA}" >&2
  exit 1
fi
