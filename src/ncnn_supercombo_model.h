#ifndef NCNN_SUPERCOMBO_MODEL_H
#define NCNN_SUPERCOMBO_MODEL_H

#include "app_config.h"
#include "model_input_history.h"
#include "model_input_transform.h"
#include "ncnn_output_contract.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ncnn {
class Net;
}

class NcnnSupercomboModel {
public:
    struct RunTiming {
        float warp_ms = 0.0f;
        float input_ms = 0.0f;
        float infer_ms = 0.0f;
        float output_ms = 0.0f;
        float total_ms = 0.0f;
    };

    NcnnSupercomboModel(const std::string &param_path, const std::string &bin_path,
                        const AppConfig &config);
    ~NcnnSupercomboModel();

    bool run_frame_nv12(const uint8_t *nv12, int src_w, int src_h,
                        std::vector<float> &raw_output, RunTiming *timing = nullptr);
    void set_input_calibration(const float rpy[3]);
    void reset_state();

private:
    static constexpr int kModelW = 512;
    static constexpr int kModelH = 256;
    static constexpr int kHalfW = kModelW / 2;
    static constexpr int kHalfH = kModelH / 2;
    static constexpr int kYuv6Floats = 6 * kHalfW * kHalfH;
    static constexpr int kInputImageFloats = 12 * kHalfW * kHalfH;
    static constexpr int kRecurrentFloats = NcnnOutputContract::kRecurrentFloats;

    bool run_current_yuv6(std::vector<float> &raw_output, RunTiming *timing);
    void prepare_input_tensor();
    void copy_output_to_float(const void *data, int elembits, size_t count,
                              std::vector<float> &raw_output) const;
    static uint16_t float_to_bfloat16_bits(float value);
    static float bfloat16_bits_to_float(uint16_t value);

    std::unique_ptr<ncnn::Net> net_;
    ModelInputTransform input_transform_;
    ModelInputHistory input_history_;
    std::vector<float> current_yuv_;
    std::vector<float> input_imgs_;
    std::vector<uint16_t> input_imgs_bf16_;
    std::vector<float> desire_;
    std::vector<float> traffic_convention_;
    std::vector<float> recurrent_state_;
    bool use_bf16_input_ = true;
};

#endif
