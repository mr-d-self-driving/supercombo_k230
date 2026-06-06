#include "app_config.h"
#include "model_output.h"
#include "ncnn_supercombo_model.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr unsigned kFrameW = 512;
constexpr unsigned kFrameH = 256;
constexpr size_t kFrameBytes = kFrameW * kFrameH * 3 / 2;

unsigned env_u32(const char *name, unsigned default_value)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end == value ? default_value : static_cast<unsigned>(parsed);
}

void fill_synthetic_nv12(std::vector<uint8_t> &frame, unsigned frame_id)
{
    frame.resize(kFrameBytes);
    uint8_t *y = frame.data();
    uint8_t *uv = y + kFrameW * kFrameH;
    for (unsigned yy = 0; yy < kFrameH; ++yy) {
        for (unsigned xx = 0; xx < kFrameW; ++xx)
            y[yy * kFrameW + xx] = static_cast<uint8_t>((xx + yy + frame_id * 3) & 0xff);
    }
    for (unsigned yy = 0; yy < kFrameH / 2; ++yy) {
        for (unsigned xx = 0; xx < kFrameW; xx += 2) {
            uv[yy * kFrameW + xx + 0] = static_cast<uint8_t>(96 + ((xx + frame_id) & 31));
            uv[yy * kFrameW + xx + 1] = static_cast<uint8_t>(128 + ((yy + frame_id) & 31));
        }
    }
}

class ReplayNv12Reader {
public:
    explicit ReplayNv12Reader(const std::string &path)
        : path_(path)
    {
        file_.open(path_, std::ios::binary);
        if (!file_) throw std::runtime_error("open replay failed: " + path_);

        char magic[8] {};
        read_exact(magic, sizeof(magic), "magic");
        if (std::memcmp(magic, "SCNV12R1", sizeof(magic)) != 0)
            throw std::runtime_error("bad replay magic: " + path_);
        read_exact(reinterpret_cast<char *>(&width_), sizeof(width_), "width");
        read_exact(reinterpret_cast<char *>(&height_), sizeof(height_), "height");
        read_exact(reinterpret_cast<char *>(&frame_count_), sizeof(frame_count_), "frame_count");
        if (width_ != kFrameW || height_ != kFrameH)
            throw std::runtime_error("replay must be 512x256 NV12: " + path_);
    }

    bool read(std::vector<uint8_t> &frame)
    {
        if (frames_read_ >= frame_count_) return false;
        frame.resize(kFrameBytes);
        file_.read(reinterpret_cast<char *>(frame.data()), static_cast<std::streamsize>(frame.size()));
        if (file_.gcount() != static_cast<std::streamsize>(frame.size()))
            throw std::runtime_error("short replay frame read: " + path_);
        ++frames_read_;
        return true;
    }

private:
    void read_exact(char *dst, size_t size, const char *label)
    {
        file_.read(dst, static_cast<std::streamsize>(size));
        if (file_.gcount() != static_cast<std::streamsize>(size))
            throw std::runtime_error(std::string("short replay header read: ") + label);
    }

    std::string path_;
    std::ifstream file_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    unsigned frame_count_ = 0;
    unsigned frames_read_ = 0;
};

double point_l2(const ModelPoint &a, const ModelPoint &b)
{
    const double dx = static_cast<double>(a.x) - b.x;
    const double dy = static_cast<double>(a.y) - b.y;
    const double dz = static_cast<double>(a.z) - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct DiffStats {
    unsigned frames = 0;
    size_t samples = 0;
    double raw_sum_abs = 0.0;
    double raw_max_abs = 0.0;
    double plan_sum_l2 = 0.0;
    double plan_end_sum_l2 = 0.0;
    double lane_y_sum_abs = 0.0;
    double lane_y_max_abs = 0.0;
    double road_edge_y_sum_abs = 0.0;
    double road_edge_y_max_abs = 0.0;
    unsigned best_plan_mismatch = 0;

    void add(const std::vector<float> &float_raw, const std::vector<float> &bf16_raw)
    {
        if (float_raw.size() != bf16_raw.size())
            throw std::runtime_error("raw output sizes differ");
        ++frames;
        samples += float_raw.size();
        for (size_t i = 0; i < float_raw.size(); ++i) {
            const double diff = std::fabs(static_cast<double>(float_raw[i]) - bf16_raw[i]);
            raw_sum_abs += diff;
            raw_max_abs = std::max(raw_max_abs, diff);
        }

        const ParsedModelOutput float_parsed = ModelOutputParser::parse(float_raw);
        const ParsedModelOutput bf16_parsed = ModelOutputParser::parse(bf16_raw);
        if (float_parsed.plan.best_index != bf16_parsed.plan.best_index)
            ++best_plan_mismatch;

        for (int i = 0; i < kTrajectorySize; ++i) {
            plan_sum_l2 += point_l2(float_parsed.plan.points[i], bf16_parsed.plan.points[i]);
            if (i == kTrajectorySize - 1)
                plan_end_sum_l2 += point_l2(float_parsed.plan.points[i], bf16_parsed.plan.points[i]);

            for (int lane = 0; lane < 4; ++lane) {
                const double diff = std::fabs(static_cast<double>(float_parsed.lanes[lane].points[i].y) -
                                              bf16_parsed.lanes[lane].points[i].y);
                lane_y_sum_abs += diff;
                lane_y_max_abs = std::max(lane_y_max_abs, diff);
            }
            for (int edge = 0; edge < 2; ++edge) {
                const double diff = std::fabs(static_cast<double>(float_parsed.road_edges[edge].points[i].y) -
                                              bf16_parsed.road_edges[edge].points[i].y);
                road_edge_y_sum_abs += diff;
                road_edge_y_max_abs = std::max(road_edge_y_max_abs, diff);
            }
        }
    }

    void print(const char *source) const
    {
        const double raw_mean = samples > 0 ? raw_sum_abs / static_cast<double>(samples) : 0.0;
        const double plan_mean_l2 = frames > 0
            ? plan_sum_l2 / static_cast<double>(frames * kTrajectorySize)
            : 0.0;
        const double plan_end_l2 = frames > 0
            ? plan_end_sum_l2 / static_cast<double>(frames)
            : 0.0;
        const double lane_y_mean = frames > 0
            ? lane_y_sum_abs / static_cast<double>(frames * 4 * kTrajectorySize)
            : 0.0;
        const double edge_y_mean = frames > 0
            ? road_edge_y_sum_abs / static_cast<double>(frames * 2 * kTrajectorySize)
            : 0.0;
        const bool accuracy_ok = plan_end_l2 <= 1.0 && lane_y_max_abs <= 0.25 &&
            road_edge_y_max_abs <= 0.5 && best_plan_mismatch == 0;

        std::printf("NCNN_INPUT_BF16_COMPARE result=PASS source=%s frames=%u raw_mean_abs=%.8f raw_max_abs=%.8f "
                    "plan_mean_l2=%.8f plan_end_l2=%.8f lane_y_mean_abs=%.8f lane_y_max_abs=%.8f "
                    "road_edge_y_mean_abs=%.8f road_edge_y_max_abs=%.8f best_plan_mismatch=%u "
                    "accuracy=%s recommended_input=%s\n",
                    source, frames, raw_mean, raw_max_abs, plan_mean_l2, plan_end_l2,
                    lane_y_mean, lane_y_max_abs, edge_y_mean, road_edge_y_max_abs,
                    best_plan_mismatch,
                    accuracy_ok ? "PASS" : "FAIL",
                    accuracy_ok ? "bf16" : "float");
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <model.param> <model.bin>\n", argc > 0 ? argv[0] : "compare_ncnn_input_bf16");
        return 2;
    }

    try {
        const unsigned frames = env_u32("COMPARE_FRAMES", 20);
        const char *replay_path = std::getenv("COMPARE_REPLAY_NV12");
        const AppConfig config = AppConfig::from_env_defaults(argc > 0 ? argv[0] : "compare_ncnn_input_bf16");

        setenv("RPI_NCNN_INPUT_BF16", "0", 1);
        NcnnSupercomboModel float_model(argv[1], argv[2], config);
        setenv("RPI_NCNN_INPUT_BF16", "1", 1);
        NcnnSupercomboModel bf16_model(argv[1], argv[2], config);

        DiffStats stats;
        std::vector<uint8_t> frame;
        std::unique_ptr<ReplayNv12Reader> replay;
        if (replay_path && replay_path[0] != '\0')
            replay.reset(new ReplayNv12Reader(replay_path));
        for (unsigned i = 0; i < frames; ++i) {
            if (replay) {
                if (!replay->read(frame)) break;
            } else {
                fill_synthetic_nv12(frame, i);
            }
            std::vector<float> float_raw;
            std::vector<float> bf16_raw;
            if (!float_model.run_frame_nv12(frame.data(), kFrameW, kFrameH, float_raw))
                throw std::runtime_error("float input model run failed");
            if (!bf16_model.run_frame_nv12(frame.data(), kFrameW, kFrameH, bf16_raw))
                throw std::runtime_error("bf16 input model run failed");
            stats.add(float_raw, bf16_raw);
        }
        if (stats.frames == 0)
            throw std::runtime_error("no frames compared");
        stats.print(replay ? "replay" : "synthetic");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "NCNN_INPUT_BF16_COMPARE result=FAIL error=%s\n", e.what());
        return 1;
    }
}
