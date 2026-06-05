#include "ncnn_supercombo_model.h"

#include "cpu.h"
#include "net.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace {

bool env_enabled_local(const char *name, bool default_value)
{
    const char *value = std::getenv(name);
    if (!value) return default_value;
    return value[0] != '\0' && std::strcmp(value, "0") != 0;
}

int env_int_local(const char *name, int default_value)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value ? default_value : static_cast<int>(parsed);
}

uint64_t steady_ns_local()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

float elapsed_ms(uint64_t begin_ns, uint64_t end_ns)
{
    return static_cast<float>((end_ns - begin_ns) / 1000000.0);
}

} // namespace

NcnnSupercomboModel::NcnnSupercomboModel(const std::string &param_path,
                                         const std::string &bin_path,
                                         const AppConfig &config)
    : net_(new ncnn::Net()),
      input_transform_(config),
      prev_yuv_(kYuv6Floats, 0.0f),
      current_yuv_(kYuv6Floats, 0.0f),
      input_imgs_(kInputImageFloats, 0.0f),
      input_imgs_bf16_(kInputImageFloats, 0),
      desire_(8, 0.0f),
      traffic_convention_{1.0f, 0.0f},
      recurrent_state_(kRecurrentFloats, 0.0f),
      use_bf16_input_(env_enabled_local("RPI_NCNN_INPUT_BF16", false))
{
    ncnn::set_omp_dynamic(0);
    ncnn::set_kmp_blocktime(env_int_local("RPI_NCNN_BLOCKTIME", 0));

    net_->opt.num_threads = env_int_local("RPI_NCNN_THREADS", 4);
    net_->opt.openmp_blocktime = env_int_local("RPI_NCNN_BLOCKTIME", 0);
    net_->opt.use_vulkan_compute = false;
    net_->opt.use_packing_layout = env_enabled_local("RPI_NCNN_PACKING", true);
    net_->opt.use_winograd_convolution = env_enabled_local("RPI_NCNN_WINOGRAD", true);
    net_->opt.use_winograd23_convolution = env_enabled_local("RPI_NCNN_WINOGRAD", true);
    net_->opt.use_winograd43_convolution = env_enabled_local("RPI_NCNN_WINOGRAD", true);
    net_->opt.use_winograd63_convolution = env_enabled_local("RPI_NCNN_WINOGRAD", true);
    net_->opt.use_sgemm_convolution = env_enabled_local("RPI_NCNN_SGEMM", false);
    net_->opt.use_fp16_storage = false;
    net_->opt.use_fp16_arithmetic = false;
    net_->opt.use_bf16_storage = env_enabled_local("RPI_NCNN_BF16", true);
    net_->opt.use_bf16_packed = env_enabled_local("RPI_NCNN_BF16", true);
    net_->opt.flush_denormals = static_cast<unsigned char>(env_int_local("RPI_NCNN_FLUSH_DENORMALS", 2));
    net_->opt.use_local_pool_allocator = env_enabled_local("RPI_NCNN_LOCAL_POOL", false);
    net_->opt.use_a53_a55_optimized_kernel = false;

    int ret = net_->load_param(param_path.c_str());
    if (ret != 0)
        throw std::runtime_error("ncnn load_param failed: " + param_path);
    ret = net_->load_model(bin_path.c_str());
    if (ret != 0)
        throw std::runtime_error("ncnn load_model failed: " + bin_path);

    std::fprintf(stderr,
                 "rpi ncnn model loaded param=%s bin=%s threads=%d bf16=%d input_bf16=%d\n",
                 param_path.c_str(), bin_path.c_str(), net_->opt.num_threads,
                 net_->opt.use_bf16_storage ? 1 : 0, use_bf16_input_ ? 1 : 0);
}

NcnnSupercomboModel::~NcnnSupercomboModel() = default;

void NcnnSupercomboModel::reset_state()
{
    std::fill(prev_yuv_.begin(), prev_yuv_.end(), 0.0f);
    std::fill(recurrent_state_.begin(), recurrent_state_.end(), 0.0f);
}

void NcnnSupercomboModel::set_input_calibration(const float rpy[3])
{
    input_transform_.set_calibration(rpy[0], rpy[1], rpy[2]);
}

uint16_t NcnnSupercomboModel::float_to_bfloat16_bits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return static_cast<uint16_t>(bits >> 16);
}

float NcnnSupercomboModel::bfloat16_bits_to_float(uint16_t value)
{
    const uint32_t bits = static_cast<uint32_t>(value) << 16;
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

void NcnnSupercomboModel::prepare_input_tensor()
{
    std::memcpy(input_imgs_.data(), prev_yuv_.data(), prev_yuv_.size() * sizeof(float));
    std::memcpy(input_imgs_.data() + prev_yuv_.size(), current_yuv_.data(),
                current_yuv_.size() * sizeof(float));
    prev_yuv_.swap(current_yuv_);

    if (use_bf16_input_) {
        for (size_t i = 0; i < input_imgs_.size(); ++i)
            input_imgs_bf16_[i] = float_to_bfloat16_bits(input_imgs_[i]);
    }
}

void NcnnSupercomboModel::copy_output_to_float(const void *data, int elembits, size_t count,
                                               std::vector<float> &raw_output) const
{
    raw_output.resize(count);
    if (elembits == 16) {
        const uint16_t *src = static_cast<const uint16_t *>(data);
        for (size_t i = 0; i < count; ++i)
            raw_output[i] = bfloat16_bits_to_float(src[i]);
    } else {
        std::memcpy(raw_output.data(), data, count * sizeof(float));
    }
}

bool NcnnSupercomboModel::run_frame_nv12(const uint8_t *nv12, int src_w, int src_h,
                                         std::vector<float> &raw_output, RunTiming *timing)
{
    if (timing) *timing = RunTiming {};
    const uint64_t t0 = steady_ns_local();
    input_transform_.nv12_to_yuv6_warped(nv12, src_w, src_h, current_yuv_);
    const uint64_t t1 = steady_ns_local();
    if (timing) timing->warp_ms = elapsed_ms(t0, t1);
    const bool ok = run_current_yuv6(raw_output, timing);
    const uint64_t t2 = steady_ns_local();
    if (timing) timing->total_ms = elapsed_ms(t0, t2);
    return ok;
}

bool NcnnSupercomboModel::run_current_yuv6(std::vector<float> &raw_output, RunTiming *timing)
{
    const uint64_t input_t0 = steady_ns_local();
    prepare_input_tensor();

    ncnn::Mat input_imgs(256, 128, 12, use_bf16_input_
        ? static_cast<void *>(input_imgs_bf16_.data())
        : static_cast<void *>(input_imgs_.data()),
        use_bf16_input_ ? static_cast<size_t>(2u) : static_cast<size_t>(4u),
        1);
    ncnn::Mat desire(8, static_cast<void *>(desire_.data()));
    ncnn::Mat traffic(2, static_cast<void *>(traffic_convention_.data()));
    ncnn::Mat initial_state(512, static_cast<void *>(recurrent_state_.data()));

    ncnn::Extractor extractor = net_->create_extractor();
    extractor.set_light_mode(env_enabled_local("RPI_NCNN_LIGHTMODE", true));
    if (extractor.input("input_imgs", input_imgs) != 0) return false;
    if (extractor.input("desire", desire) != 0) return false;
    if (extractor.input("traffic_convention", traffic) != 0) return false;
    if (extractor.input("initial_state", initial_state) != 0) return false;
    const uint64_t input_t1 = steady_ns_local();
    if (timing) timing->input_ms = elapsed_ms(input_t0, input_t1);

    ncnn::Mat output;
    const uint64_t infer_t0 = steady_ns_local();
    if (extractor.extract("outputs", output) != 0) return false;
    const uint64_t infer_t1 = steady_ns_local();
    if (timing) timing->infer_ms = elapsed_ms(infer_t0, infer_t1);

    const uint64_t output_t0 = steady_ns_local();
    std::vector<float> full_output;
    copy_output_to_float(output.data, output.elembits(), output.total(), full_output);

    if (full_output.size() >= recurrent_state_.size()) {
        std::memcpy(recurrent_state_.data(),
                    full_output.data() + full_output.size() - recurrent_state_.size(),
                    recurrent_state_.size() * sizeof(float));
    }
    if (full_output.size() == kPrunedVizOutputFloats) {
        raw_output.assign(full_output.begin(), full_output.begin() + kPrunedVizParserFloats);
    } else {
        raw_output.swap(full_output);
    }
    const uint64_t output_t1 = steady_ns_local();
    if (timing) timing->output_ms = elapsed_ms(output_t0, output_t1);
    return true;
}
