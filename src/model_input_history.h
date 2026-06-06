#ifndef MODEL_INPUT_HISTORY_H
#define MODEL_INPUT_HISTORY_H

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

class ModelInputHistory {
public:
    static constexpr int kModelW = 512;
    static constexpr int kModelH = 256;
    static constexpr int kHalfW = kModelW / 2;
    static constexpr int kHalfH = kModelH / 2;
    static constexpr int kYuv6Floats = 6 * kHalfW * kHalfH;
    static constexpr int kInputImageFloats = 12 * kHalfW * kHalfH;

    ModelInputHistory()
        : prev_yuv_(kYuv6Floats, 0.0f)
    {
    }

    void reset()
    {
        std::fill(prev_yuv_.begin(), prev_yuv_.end(), 0.0f);
    }

    void push_yuv6(std::vector<float> &current_yuv, std::vector<float> &input_imgs)
    {
        if (current_yuv.size() != kYuv6Floats)
            throw std::runtime_error("ModelInputHistory requires a 512x256 YUV6 frame");
        if (input_imgs.size() != kInputImageFloats)
            input_imgs.resize(kInputImageFloats);

        std::memcpy(input_imgs.data(), prev_yuv_.data(), prev_yuv_.size() * sizeof(float));
        std::memcpy(input_imgs.data() + prev_yuv_.size(), current_yuv.data(),
                    current_yuv.size() * sizeof(float));
        prev_yuv_.swap(current_yuv);
    }

    const std::vector<float> &previous_yuv() const { return prev_yuv_; }

private:
    std::vector<float> prev_yuv_;
};

#endif
