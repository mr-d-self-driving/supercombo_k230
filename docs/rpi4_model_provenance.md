# Raspberry Pi 4 ncnn model provenance

This records the current ncnn model pair used by the Raspberry Pi 4 runtime.

## Current deploy pair

Runtime defaults:

```text
MODEL_PARAM=/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.param
MODEL_BIN=/home/chan/supercombo_models/supercombo_no_big_drop_pruned_viz_opt.bin
```

Current hashes on the Pi:

```text
e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2  supercombo_no_big_drop_pruned_viz_opt.param
88dc46956eb5255265c9695a29dc4fba7ec6e419e5af26de137df756c3ec277b  supercombo_no_big_drop_pruned_viz_opt.bin
```

The same optimized pair also exists under `/home/chan/onnx_bench/` on the Pi and
under `/Users/chan/Documents/RPI4/` on the Mac with matching hashes.
`scripts/rpi_smoke.sh artifacts` reports the Pi deploy hashes every run.

## Source chain

Original model source:

```text
https://github.com/cwal1220/openpilot_c2/blob/master/selfdrive/modeld/models/supercombo.onnx
/Users/chan/Documents/openpilot_c2/selfdrive/modeld/models/supercombo.onnx
sha256 50c7fc8565ac69a4b9a0de122e961326820e78bf13659255a89d0ed04be030d5
git blob 749ac1287a81971aeb5c12273591b6725ac4fabb
```

Local artifact reference repo:

```text
/Users/chan/Documents/RPI4/ncnn_cpu_bf16_results
git head 5d844ffad95f37e1b6eb5b106ff7e1da74394b69
```

Important files from that repo:

```text
conversion/remove_big_image_path.py   sha256 2fcd92710a52354bcdba969e69e70eb8b096c90e45b7cf15ffd4a376b269b084
conversion/make_pruned_supercombo.py  sha256 1b0c39245b221b8e80542e2fd7133b9afafaaad1b99a81ab0617dc420835db36
models/supercombo_no_big_drop.onnx    sha256 e912010c4f1d045d9915a4fe57690682d89c6db7d0256fedc0f54c28546c98de
```

Pi-side source ONNX files:

```text
/home/chan/onnx_bench/supercombo_no_big_drop.onnx
sha256 e912010c4f1d045d9915a4fe57690682d89c6db7d0256fedc0f54c28546c98de

/home/chan/onnx_bench/supercombo_no_big_drop_pruned_viz.onnx
sha256 60963ef873ed6769ba89d6e52938801abb41662a7ff7d30e57abc9deb4db33a3
```

Mac-side generated files:

```text
/Users/chan/Documents/RPI4/supercombo_no_big_drop_pruned_viz.onnx
sha256 60963ef873ed6769ba89d6e52938801abb41662a7ff7d30e57abc9deb4db33a3

/Users/chan/Documents/RPI4/supercombo_no_big_drop_pruned_viz_opt.param
sha256 e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2

/Users/chan/Documents/RPI4/supercombo_no_big_drop_pruned_viz_opt.bin
sha256 88dc46956eb5255265c9695a29dc4fba7ec6e419e5af26de137df756c3ec277b
```

ncnn source used for the current Pi build:

```text
/home/chan/onnx_bench/ncnn-src
git head 882f319defcdd29440eabff7bc6e493c913f29e7
describe 882f319
```

Relevant ncnn build cache values on the Pi:

```text
CMAKE_BUILD_TYPE=Release
CMAKE_CXX_FLAGS=-O3 -mcpu=cortex-a72 -mtune=cortex-a72 -ffast-math -fomit-frame-pointer
CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG
NCNN_BF16=ON
NCNN_OPENMP=ON
NCNN_VULKAN=OFF
NCNN_BUILD_TOOLS=OFF
```

## Conversion flow

The documented flow is:

```sh
python3 conversion/remove_big_image_path.py \
  --input supercombo.onnx \
  --output supercombo_no_big_drop.onnx

python3 conversion/make_pruned_supercombo.py

onnx2ncnn \
  supercombo_no_big_drop_pruned_viz.onnx \
  supercombo_no_big_drop_pruned_viz.param \
  supercombo_no_big_drop_pruned_viz.bin

ncnnoptimize \
  supercombo_no_big_drop_pruned_viz.param \
  supercombo_no_big_drop_pruned_viz.bin \
  supercombo_no_big_drop_pruned_viz_opt.param \
  supercombo_no_big_drop_pruned_viz_opt.bin \
  0
```

The deployed pair was reproduced byte-identically on the Pi from the recorded
`supercombo_no_big_drop_pruned_viz.onnx` using ncnn source
`882f319defcdd29440eabff7bc6e493c913f29e7` and `ncnnoptimize` flag `0`.

The tool rebuild used:

```text
onnx2ncnn:
  /home/chan/onnx_bench/ncnn-build-onnx2ncnn-repro/build/onnx/onnx2ncnn
ncnnoptimize:
  /tmp/ncnn-tools-a72/tools/ncnnoptimize
```

Reproduction command:

```sh
scripts/reproduce_rpi_ncnn_model.sh
```

Equivalent expanded command:

```sh
onnx2ncnn \
  /home/chan/onnx_bench/supercombo_no_big_drop_pruned_viz.onnx \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz.param \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz.bin

ncnnoptimize \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz.param \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz.bin \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz_opt.param \
  /home/chan/onnx_bench/ncnn-repro-model/supercombo_no_big_drop_pruned_viz_opt.bin \
  0
```

Verification result:

```text
NCNN_REPRO result=MATCH flag=0
  param e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2
  bin   88dc46956eb5255265c9695a29dc4fba7ec6e419e5af26de137df756c3ec277b

NCNN_REPRO result=MISS flag=1
  param e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2
  bin   8b8931050fdd54015a9f55ddb5a524accf4df37ac18cc37d508371b94d75051e

NCNN_REPRO result=MISS flag=65536
  param e3c588c6725a950b057ed7fa51559b16b5b306e5c6934c86391722915226b8c2
  bin   8b8931050fdd54015a9f55ddb5a524accf4df37ac18cc37d508371b94d75051e
```

Current ncnn source shows `flag == 65536` or `flag == 1` selects
`storage_type = 1`; any other flag selects `storage_type = 0`. The deployed
model uses the `storage_type = 0` path.

## Runtime BF16 note

The deploy pair is not a separate BF16 model file. BF16 is controlled by ncnn
runtime options in `src/ncnn_supercombo_model.cc`:

```text
use_bf16_storage = true
use_bf16_packed = true
```

`RPI_NCNN_INPUT_BF16=1` remains a performance comparison option, not the default.
