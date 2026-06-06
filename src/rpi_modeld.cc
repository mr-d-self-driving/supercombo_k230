#include "app_config.h"
#include "calibration_service.h"
#include "k230_ipc.h"
#include "lateral_control.h"
#include "model_output.h"
#include "ncnn_supercombo_model.h"

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void signal_handler(int)
{
    g_stop = 1;
}

struct TimingStats {
    unsigned count = 0;
    double warp_ms = 0.0;
    double input_ms = 0.0;
    double infer_ms = 0.0;
    double output_ms = 0.0;
    double total_ms = 0.0;

    void add(const NcnnSupercomboModel::RunTiming &timing)
    {
        ++count;
        warp_ms += timing.warp_ms;
        input_ms += timing.input_ms;
        infer_ms += timing.infer_ms;
        output_ms += timing.output_ms;
        total_ms += timing.total_ms;
    }

    void print(const char *mode) const
    {
        if (count == 0) return;
        const double scale = 1.0 / static_cast<double>(count);
        std::fprintf(stderr,
                     "rpi_modeld profile mode=%s frames=%u avg_ms total=%.2f warp=%.2f input=%.2f infer=%.2f output=%.2f\n",
                     mode, count, total_ms * scale, warp_ms * scale, input_ms * scale,
                     infer_ms * scale, output_ms * scale);
    }
};

uint64_t timeval_us(const timeval &tv)
{
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + tv.tv_usec;
}

unsigned env_u32_local(const char *name, unsigned default_value)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end == value ? default_value : static_cast<unsigned>(parsed);
}

struct Nv12Frame {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint8_t> data;
};

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
        if (width_ == 0 || height_ == 0 || (width_ & 1) || (height_ & 1))
            throw std::runtime_error("bad replay dimensions: " + path_);
        frame_bytes_ = static_cast<size_t>(width_) * height_ * 3 / 2;
    }

    bool read(Nv12Frame &frame)
    {
        if (frames_read_ >= frame_count_) return false;
        frame.width = width_;
        frame.height = height_;
        frame.data.resize(frame_bytes_);
        file_.read(reinterpret_cast<char *>(frame.data.data()), static_cast<std::streamsize>(frame.data.size()));
        if (file_.gcount() != static_cast<std::streamsize>(frame.data.size()))
            throw std::runtime_error("short replay frame read: " + path_);
        ++frames_read_;
        return true;
    }

    unsigned frame_count() const { return frame_count_; }

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
    size_t frame_bytes_ = 0;
};

void fill_synthetic_nv12(Nv12Frame &frame, uint64_t frame_id)
{
    frame.width = kK230AiWidth;
    frame.height = kK230AiHeight;
    frame.data.resize(kK230AiFrameBytes);
    uint8_t *y = frame.data.data();
    uint8_t *uv = y + frame.width * frame.height;
    for (unsigned yy = 0; yy < frame.height; ++yy) {
        for (unsigned xx = 0; xx < frame.width; ++xx)
            y[yy * frame.width + xx] = static_cast<uint8_t>((xx + yy + frame_id * 3) & 0xff);
    }
    for (unsigned yy = 0; yy < frame.height / 2; ++yy) {
        for (unsigned xx = 0; xx < frame.width; xx += 2) {
            uv[yy * frame.width + xx + 0] = static_cast<uint8_t>(96 + ((xx + frame_id) & 31));
            uv[yy * frame.width + xx + 1] = static_cast<uint8_t>(128 + ((yy + frame_id) & 31));
        }
    }
}

bool publish_output(K230LatestChannel &model_pub, NcnnSupercomboModel &model,
                    const ParsedModelOutput &parsed, CalibrationService &calibration,
                    LateralControlDraft &lateral_control, uint64_t frame_id,
                    uint64_t capture_timestamp_ns, float model_ms)
{
    calibration.update(parsed);
    float input_rpy[3];
    calibration.input_rpy(input_rpy);
    model.set_input_calibration(input_rpy);

    const ProjectionState projection = calibration.projection();
    const LateralTarget lateral = lateral_control.update(parsed.plan, projection);

    K230ModelState state;
    k230_fill_model_state(state, parsed, projection, calibration.snapshot(), lateral,
                          frame_id, capture_timestamp_ns, model_ms);
    return model_pub.publish(&state, sizeof(state));
}

int run_one_nv12(NcnnSupercomboModel &model, K230LatestChannel &model_pub,
                 CalibrationService &calibration, LateralControlDraft &lateral_control,
                 const uint8_t *nv12, unsigned width, unsigned height,
                 uint64_t frame_id, unsigned &errors, float *model_ms_out,
                 NcnnSupercomboModel::RunTiming *timing_out)
{
    std::vector<float> raw;
    NcnnSupercomboModel::RunTiming timing;
    const bool ok = model.run_frame_nv12(nv12, static_cast<int>(width), static_cast<int>(height), raw, &timing);
    const float model_ms = timing.total_ms;
    if (model_ms_out) *model_ms_out = model_ms;
    if (timing_out) *timing_out = timing;
    if (!ok) {
        ++errors;
        return 1;
    }

    ParsedModelOutput parsed = ModelOutputParser::parse(raw);
    if (!publish_output(model_pub, model, parsed, calibration, lateral_control,
                        frame_id, k230_now_ns(), model_ms)) {
        std::fprintf(stderr, "\nrpi_modeld: publish modelState failed\n");
        ++errors;
        return 1;
    }
    return 0;
}

int run_one_frame(NcnnSupercomboModel &model, K230LatestChannel &model_pub,
                  CalibrationService &calibration, LateralControlDraft &lateral_control,
                  const Nv12Frame &frame, uint64_t frame_id, unsigned &errors,
                  float *model_ms_out, NcnnSupercomboModel::RunTiming *timing_out)
{
    return run_one_nv12(model, model_pub, calibration, lateral_control, frame.data.data(),
                        frame.width, frame.height, frame_id, errors, model_ms_out, timing_out);
}

int run_synthetic(const AppConfig &config, NcnnSupercomboModel &model,
                  K230LatestChannel &model_pub, CalibrationService &calibration,
                  LateralControlDraft &lateral_control)
{
    const unsigned frames = env_u32_local("RPI_SYNTHETIC_FRAMES",
        config.max_frames > 0 ? config.max_frames : 30);
    unsigned errors = 0;
    const bool profile = env_u32_local("RPI_PROFILE_MODEL", 0) != 0;
    TimingStats timing_stats;
    timeval start {};
    gettimeofday(&start, nullptr);

    for (unsigned i = 0; !g_stop && i < frames; ++i) {
        Nv12Frame frame;
        fill_synthetic_nv12(frame, i);
        float model_ms = 0.0f;
        NcnnSupercomboModel::RunTiming timing;
        run_one_frame(model, model_pub, calibration, lateral_control, frame, i, errors, &model_ms, &timing);
        if (profile) timing_stats.add(timing);
        if (profile) {
            std::fprintf(stderr,
                         "rpi_modeld synthetic: frame=%u/%u model_ms=%.2f warp=%.2f input=%.2f infer=%.2f output=%.2f errors=%u          \r",
                         i + 1, frames, model_ms, timing.warp_ms, timing.input_ms,
                         timing.infer_ms, timing.output_ms, errors);
        } else {
            std::fprintf(stderr, "rpi_modeld synthetic: frame=%u/%u model_ms=%.2f errors=%u          \r",
                         i + 1, frames, model_ms, errors);
        }
        std::fflush(stderr);
    }

    timeval end {};
    gettimeofday(&end, nullptr);
    const uint64_t duration = timeval_us(end) - timeval_us(start);
    const double fps = duration > 0 ? frames * 1000000.0 / duration : 0.0;
    if (profile) timing_stats.print("synthetic");
    std::fprintf(stderr, "\nrpi_modeld synthetic done frames=%u errors=%u fps=%.2f\n",
                 frames, errors, fps);
    return errors == 0 ? 0 : 1;
}

int run_replay(const AppConfig &config, NcnnSupercomboModel &model,
               K230LatestChannel &model_pub, CalibrationService &calibration,
               LateralControlDraft &lateral_control)
{
    ReplayNv12Reader reader(config.replay_nv12_path);
    const unsigned target_frames = config.max_frames > 0
        ? std::min(config.max_frames, reader.frame_count())
        : reader.frame_count();
    std::fprintf(stderr, "rpi_modeld replay file=%s frames=%u target=%u\n",
                 config.replay_nv12_path.c_str(), reader.frame_count(), target_frames);

    unsigned processed = 0;
    unsigned errors = 0;
    const bool profile = env_u32_local("RPI_PROFILE_MODEL", 0) != 0;
    TimingStats timing_stats;
    timeval start {};
    gettimeofday(&start, nullptr);

    Nv12Frame frame;
    while (!g_stop && processed < target_frames && reader.read(frame)) {
        float model_ms = 0.0f;
        NcnnSupercomboModel::RunTiming timing;
        run_one_frame(model, model_pub, calibration, lateral_control, frame, processed, errors, &model_ms, &timing);
        ++processed;
        if (profile) timing_stats.add(timing);
        if (profile) {
            std::fprintf(stderr,
                         "rpi_modeld replay: frame=%u/%u model_ms=%.2f warp=%.2f input=%.2f infer=%.2f output=%.2f errors=%u          \r",
                         processed, target_frames, model_ms, timing.warp_ms, timing.input_ms,
                         timing.infer_ms, timing.output_ms, errors);
        } else {
            std::fprintf(stderr, "rpi_modeld replay: frame=%u/%u model_ms=%.2f errors=%u          \r",
                         processed, target_frames, model_ms, errors);
        }
        std::fflush(stderr);
    }

    timeval end {};
    gettimeofday(&end, nullptr);
    const uint64_t duration = timeval_us(end) - timeval_us(start);
    const double fps = duration > 0 ? processed * 1000000.0 / duration : 0.0;
    if (profile) timing_stats.print("replay");
    std::fprintf(stderr, "\nrpi_modeld replay done frames=%u errors=%u fps=%.2f\n",
                 processed, errors, fps);
    return processed > 0 && errors == 0 ? 0 : 1;
}

int run_live(const AppConfig &config, NcnnSupercomboModel &model,
             K230LatestChannel &model_pub, CalibrationService &calibration,
             LateralControlDraft &lateral_control)
{
    K230LatestChannel frame_sub;
    K230FrameRing frame_ring;
    if (!frame_sub.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), true))
        throw std::runtime_error("open roadAiFrame ipc failed");
    while (!g_stop && !frame_ring.open(false)) {
        std::fprintf(stderr, "rpi_modeld: waiting for road ai frame ring\n");
        usleep(500000);
    }
    if (!frame_ring.valid()) return 1;

    std::fprintf(stderr, "rpi_modeld live shared ring slots=%u frame=%ux%u bytes=%u\n",
                 frame_ring.slot_count(), frame_ring.width(), frame_ring.height(),
                 frame_ring.frame_bytes());

    uint64_t last_frame_seq = 0;
    uint64_t last_frame_id = 0;
    bool have_last_frame_id = false;
    unsigned processed = 0;
    unsigned errors = 0;
    unsigned missed = 0;
    const bool profile = env_u32_local("RPI_PROFILE_MODEL", 0) != 0;
    TimingStats timing_stats;
    timeval start {};
    timeval last {};
    gettimeofday(&start, nullptr);
    last = start;
    unsigned last_processed = 0;

    while (!g_stop) {
        K230RoadAiFrame meta;
        if (!frame_sub.read_new(&last_frame_seq, &meta, sizeof(meta), 1000)) {
            std::fprintf(stderr, "rpi_modeld: waiting for roadAiFrame\n");
            continue;
        }
        if (meta.slot >= frame_ring.slot_count()) {
            ++errors;
            continue;
        }
        if (have_last_frame_id && meta.frame_id > last_frame_id + 1)
            missed += static_cast<unsigned>(meta.frame_id - last_frame_id - 1);
        have_last_frame_id = true;
        last_frame_id = meta.frame_id;

        const uint8_t *nv12 = frame_ring.slot(meta.slot);
        if (!nv12) {
            ++errors;
            continue;
        }

        float model_ms = 0.0f;
        NcnnSupercomboModel::RunTiming timing;
        run_one_nv12(model, model_pub, calibration, lateral_control, nv12, meta.width, meta.height,
                     meta.frame_id, errors, &model_ms, &timing);
        ++processed;
        if (profile) timing_stats.add(timing);

        if (config.max_frames > 0 && processed >= config.max_frames) break;

        timeval now {};
        gettimeofday(&now, nullptr);
        const uint64_t duration = timeval_us(now) - timeval_us(last);
        if (duration >= 1000000ULL) {
            const unsigned frames_delta = processed - last_processed;
            if (profile) {
                std::fprintf(stderr,
                             "rpi_modeld: fps=%.2f frames=%u missed=%u errors=%u last_ms=%.2f warp=%.2f input=%.2f infer=%.2f output=%.2f          \r",
                             frames_delta * 1000000.0 / duration, processed, missed, errors,
                             model_ms, timing.warp_ms, timing.input_ms, timing.infer_ms,
                             timing.output_ms);
            } else {
                std::fprintf(stderr,
                             "rpi_modeld: fps=%.2f frames=%u missed=%u errors=%u last_ms=%.2f          \r",
                             frames_delta * 1000000.0 / duration, processed, missed, errors, model_ms);
            }
            std::fflush(stderr);
            last = now;
            last_processed = processed;
        }
    }

    timeval end {};
    gettimeofday(&end, nullptr);
    const uint64_t total_us = timeval_us(end) - timeval_us(start);
    const double fps = total_us > 0 ? processed * 1000000.0 / total_us : 0.0;
    if (profile) timing_stats.print("live");
    std::fprintf(stderr, "\nrpi_modeld done frames=%u missed=%u errors=%u fps=%.2f\n",
                 processed, missed, errors, fps);
    return processed > 0 && errors == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <model.param> <model.bin>\n", argc > 0 ? argv[0] : "rpi_modeld");
        return 2;
    }

    try {
        AppConfig config = AppConfig::from_env_defaults(argc > 0 ? argv[0] : "rpi_modeld");
        K230LatestChannel model_pub;
        if (!model_pub.open(kK230ModelStateTopic, sizeof(K230ModelState), true))
            throw std::runtime_error("open modelState ipc failed");

        NcnnSupercomboModel model(argv[1], argv[2], config);
        CalibrationService calibration(config);
        LateralControlDraft lateral_control;

        if (env_enabled("RPI_SYNTHETIC")) return run_synthetic(config, model, model_pub, calibration, lateral_control);
        if (config.replay_enabled()) return run_replay(config, model, model_pub, calibration, lateral_control);
        return run_live(config, model, model_pub, calibration, lateral_control);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "rpi_modeld error: %s\n", e.what());
        return 1;
    }
}
