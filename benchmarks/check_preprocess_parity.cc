#include "app_config.h"
#include "model_input_history.h"
#include "model_input_transform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kModelW = ModelInputHistory::kModelW;
constexpr int kModelH = ModelInputHistory::kModelH;
constexpr int kHalfW = ModelInputHistory::kHalfW;
constexpr int kHalfH = ModelInputHistory::kHalfH;
constexpr int kPlaneSize = kHalfW * kHalfH;
constexpr int kYuv6Floats = ModelInputHistory::kYuv6Floats;
constexpr int kInputImageFloats = ModelInputHistory::kInputImageFloats;
constexpr int kNv12Bytes = kModelW * kModelH * 3 / 2;

int g_failures = 0;

void fail(const char *message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

float max_abs_diff(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size()) return INFINITY;
    float max_diff = 0.0f;
    for (size_t i = 0; i < a.size(); ++i)
        max_diff = std::max(max_diff, std::fabs(a[i] - b[i]));
    return max_diff;
}

uint64_t fnv1a_floats(const float *data, size_t count)
{
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bits = 0;
        std::memcpy(&bits, data + i, sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= static_cast<uint8_t>((bits >> (byte * 8)) & 0xff);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

uint64_t fnv1a_vector(const std::vector<float> &values)
{
    return fnv1a_floats(values.data(), values.size());
}

void fill_nv12(std::vector<uint8_t> &nv12, uint64_t frame_id)
{
    nv12.resize(kNv12Bytes);
    uint8_t *y = nv12.data();
    uint8_t *uv = y + kModelW * kModelH;
    for (int yy = 0; yy < kModelH; ++yy) {
        for (int xx = 0; xx < kModelW; ++xx)
            y[yy * kModelW + xx] = static_cast<uint8_t>((xx + yy * 3 + frame_id * 7) & 0xff);
    }
    for (int yy = 0; yy < kModelH / 2; ++yy) {
        for (int xx = 0; xx < kModelW; xx += 2) {
            uv[yy * kModelW + xx + 0] = static_cast<uint8_t>(80 + ((xx + frame_id * 5) & 63));
            uv[yy * kModelW + xx + 1] = static_cast<uint8_t>(120 + ((yy * 2 + frame_id * 3) & 63));
        }
    }
}

void pack_direct_yuv6(const uint8_t *nv12, std::vector<float> &out)
{
    out.assign(kYuv6Floats, 0.0f);
    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + kModelW * kModelH;
    float *y00 = out.data();
    float *y10 = y00 + kPlaneSize;
    float *y01 = y10 + kPlaneSize;
    float *y11 = y01 + kPlaneSize;
    float *u = y11 + kPlaneSize;
    float *v = u + kPlaneSize;

    for (int y2 = 0; y2 < kHalfH; ++y2) {
        const int sy = y2 * 2;
        const uint8_t *row0 = y_plane + sy * kModelW;
        const uint8_t *row1 = row0 + kModelW;
        const uint8_t *uv = uv_plane + y2 * kModelW;
        for (int x2 = 0; x2 < kHalfW; ++x2) {
            const int sx = x2 * 2;
            const int dst = y2 * kHalfW + x2;
            y00[dst] = static_cast<float>(row0[sx + 0]);
            y10[dst] = static_cast<float>(row1[sx + 0]);
            y01[dst] = static_cast<float>(row0[sx + 1]);
            y11[dst] = static_cast<float>(row1[sx + 1]);
            u[dst] = static_cast<float>(uv[sx + 0]);
            v[dst] = static_cast<float>(uv[sx + 1]);
        }
    }
}

bool range_matches(const std::vector<float> &actual, size_t offset, const std::vector<float> &expected)
{
    if (offset + expected.size() > actual.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (actual[offset + i] != expected[i])
            return false;
    }
    return true;
}

void check_history_stack(const std::vector<float> &yuv0, const std::vector<float> &yuv1,
                         std::vector<float> &input0, std::vector<float> &input1)
{
    ModelInputHistory history;
    std::vector<float> current0 = yuv0;
    std::vector<float> current1 = yuv1;
    history.push_yuv6(current0, input0);
    history.push_yuv6(current1, input1);

    const std::vector<float> zeros(kYuv6Floats, 0.0f);
    if (!range_matches(input0, 0, zeros))
        fail("first input previous half must be zero");
    if (!range_matches(input0, kYuv6Floats, yuv0))
        fail("first input current half must be frame 0");
    if (!range_matches(input1, 0, yuv0))
        fail("second input previous half must be frame 0");
    if (!range_matches(input1, kYuv6Floats, yuv1))
        fail("second input current half must be frame 1");
}

} // namespace

int main()
{
    std::vector<uint8_t> nv12_0;
    std::vector<uint8_t> nv12_1;
    fill_nv12(nv12_0, 0);
    fill_nv12(nv12_1, 1);

    std::vector<float> direct0;
    std::vector<float> direct1;
    pack_direct_yuv6(nv12_0.data(), direct0);
    pack_direct_yuv6(nv12_1.data(), direct1);

    AppConfig identity_config;
    ModelInputTransform identity(identity_config);
    std::vector<float> warped0(kYuv6Floats, 0.0f);
    std::vector<float> warped1(kYuv6Floats, 0.0f);
    identity.nv12_to_yuv6_warped(nv12_0.data(), kModelW, kModelH, warped0);
    identity.nv12_to_yuv6_warped(nv12_1.data(), kModelW, kModelH, warped1);

    const float identity_max0 = max_abs_diff(direct0, warped0);
    const float identity_max1 = max_abs_diff(direct1, warped1);
    if (identity_max0 != 0.0f || identity_max1 != 0.0f)
        fail("zero-rpy warped YUV6 must match direct YUV6 packing exactly");

    std::vector<float> input0(kInputImageFloats, 0.0f);
    std::vector<float> input1(kInputImageFloats, 0.0f);
    check_history_stack(warped0, warped1, input0, input1);

    AppConfig nonzero_config;
    nonzero_config.input_warp_pitch = static_cast<float>(1.25 * 3.14159265358979323846 / 180.0);
    nonzero_config.input_warp_yaw = static_cast<float>(-0.75 * 3.14159265358979323846 / 180.0);
    ModelInputTransform nonzero(nonzero_config);
    std::vector<float> nonzero_yuv(kYuv6Floats, 0.0f);
    nonzero.nv12_to_yuv6_warped(nv12_0.data(), kModelW, kModelH, nonzero_yuv);
    const float nonzero_max = max_abs_diff(warped0, nonzero_yuv);
    if (nonzero_max < 1.0f)
        fail("nonzero pitch/yaw warp should change the YUV6 tensor");

    const char *result = g_failures == 0 ? "PASS" : "FAIL";
    std::printf("PREPROCESS_PARITY result=%s identity_max0=%.1f identity_max1=%.1f "
                "nonzero_max=%.1f yuv0_hash=0x%016llx yuv1_hash=0x%016llx "
                "input0_hash=0x%016llx input1_hash=0x%016llx\n",
                result, identity_max0, identity_max1, nonzero_max,
                static_cast<unsigned long long>(fnv1a_vector(warped0)),
                static_cast<unsigned long long>(fnv1a_vector(warped1)),
                static_cast<unsigned long long>(fnv1a_vector(input0)),
                static_cast<unsigned long long>(fnv1a_vector(input1)));
    return g_failures == 0 ? 0 : 1;
}
