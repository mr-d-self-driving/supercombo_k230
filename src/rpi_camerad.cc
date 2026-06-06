#include "app_config.h"
#include "k230_ipc.h"

#include <linux/videodev2.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void signal_handler(int)
{
    g_stop = 1;
}

uint64_t timeval_us(const timeval &tv)
{
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL + tv.tv_usec;
}

int env_int_local(const char *name, int default_value)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value ? default_value : static_cast<int>(parsed);
}

bool env_enabled_local(const char *name, bool default_value)
{
    const char *value = std::getenv(name);
    if (!value) return default_value;
    return value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::string camera_source()
{
    const char *value = std::getenv("RPI_CAMERA_SOURCE");
    return value && value[0] != '\0' ? std::string(value) : std::string();
}

std::string replay_source()
{
    const char *value = std::getenv("RPI_CAMERA_REPLAY_NV12");
    return value && value[0] != '\0' ? std::string(value) : std::string();
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
        if (width_ != kK230AiWidth || height_ != kK230AiHeight)
            throw std::runtime_error("RPI camerad replay requires 512x256 NV12: " + path_);
        frame_bytes_ = static_cast<size_t>(width_) * height_ * 3 / 2;
        data_offset_ = file_.tellg();
    }

    bool read(std::vector<uint8_t> &nv12)
    {
        if (frames_read_ >= frame_count_) return false;
        nv12.resize(frame_bytes_);
        file_.read(reinterpret_cast<char *>(nv12.data()), static_cast<std::streamsize>(nv12.size()));
        if (file_.gcount() != static_cast<std::streamsize>(nv12.size()))
            throw std::runtime_error("short replay frame read: " + path_);
        ++frames_read_;
        return true;
    }

    void rewind()
    {
        file_.clear();
        file_.seekg(data_offset_);
        frames_read_ = 0;
    }

    unsigned frame_count() const { return frame_count_; }
    unsigned frames_read() const { return frames_read_; }

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
    std::streampos data_offset_ = 0;
};

cv::Rect center_crop_2to1(const cv::Mat &frame)
{
    int crop_w = frame.cols;
    int crop_h = frame.cols / 2;
    if (crop_h > frame.rows) {
        crop_h = frame.rows;
        crop_w = frame.rows * 2;
    }
    crop_w = std::max(2, crop_w & ~1);
    crop_h = std::max(2, crop_h & ~1);
    const int x = std::max(0, (frame.cols - crop_w) / 2);
    const int y = std::max(0, (frame.rows - crop_h) / 2);
    return cv::Rect(x, y, std::min(crop_w, frame.cols - x), std::min(crop_h, frame.rows - y));
}

void bgr_to_nv12_512x256(const cv::Mat &bgr, std::vector<uint8_t> &nv12, cv::Rect *used_crop)
{
    cv::Mat resized;
    const cv::Rect crop = center_crop_2to1(bgr);
    if (used_crop) *used_crop = crop;
    cv::resize(bgr(crop), resized, cv::Size(kK230AiWidth, kK230AiHeight), 0.0, 0.0, cv::INTER_AREA);

    cv::Mat i420;
    cv::cvtColor(resized, i420, cv::COLOR_BGR2YUV_I420);

    nv12.resize(kK230AiFrameBytes);
    const size_t y_size = static_cast<size_t>(kK230AiWidth) * kK230AiHeight;
    const size_t uv_plane_size = y_size / 4;
    std::memcpy(nv12.data(), i420.data, y_size);

    const uint8_t *u = i420.data + y_size;
    const uint8_t *v = u + uv_plane_size;
    uint8_t *uv = nv12.data() + y_size;
    for (size_t i = 0; i < uv_plane_size; ++i) {
        uv[i * 2 + 0] = u[i];
        uv[i * 2 + 1] = v[i];
    }
}

void fill_synthetic_nv12(std::vector<uint8_t> &nv12, uint64_t frame_id)
{
    nv12.resize(kK230AiFrameBytes);
    uint8_t *y = nv12.data();
    uint8_t *uv = y + kK230AiWidth * kK230AiHeight;
    for (unsigned yy = 0; yy < kK230AiHeight; ++yy) {
        for (unsigned xx = 0; xx < kK230AiWidth; ++xx)
            y[yy * kK230AiWidth + xx] = static_cast<uint8_t>((xx + yy + frame_id * 3) & 0xff);
    }
    for (unsigned yy = 0; yy < kK230AiHeight / 2; ++yy) {
        for (unsigned xx = 0; xx < kK230AiWidth; xx += 2) {
            uv[yy * kK230AiWidth + xx + 0] = static_cast<uint8_t>(96 + ((xx + frame_id) & 31));
            uv[yy * kK230AiWidth + xx + 1] = static_cast<uint8_t>(128 + ((yy + frame_id) & 31));
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        AppConfig config = AppConfig::from_env_defaults(argv[0]);
        K230LatestChannel frame_pub;
        K230FrameRing frame_ring;
        if (!frame_pub.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), true))
            throw std::runtime_error("open roadAiFrame ipc failed");
        if (!frame_ring.open(true, kK230AiWidth, kK230AiHeight, kK230FrameSlots))
            throw std::runtime_error("open road ai frame ring failed");

        const std::string replay_path = replay_source();
        const bool replay = !replay_path.empty();
        const bool replay_loop = replay && env_enabled_local("RPI_CAMERA_REPLAY_LOOP", false);
        const bool synthetic = !replay && env_enabled_local("RPI_CAMERA_SYNTHETIC", false);
        std::string live_source_kind = "camera";
        cv::VideoCapture cap;
        std::unique_ptr<ReplayNv12Reader> replay_reader;
        if (replay)
            replay_reader.reset(new ReplayNv12Reader(replay_path));
        int request_w = env_int_local("RPI_CAMERA_CAPTURE_W", 1280);
        int request_h = env_int_local("RPI_CAMERA_CAPTURE_H", 720);
        const int request_fps = env_int_local("RPI_CAMERA_FPS", 30);
        const int max_read_errors = env_int_local("RPI_CAMERA_MAX_READ_ERRORS", 90);
        if (!synthetic && !replay) {
            const std::string source = camera_source();
            std::string source_desc;
            if (!source.empty()) {
                const bool is_device = source.find("/dev/video") == 0;
                cap.open(source, is_device ? cv::CAP_V4L2 : cv::CAP_ANY);
                live_source_kind = is_device ? "camera" : "file";
                source_desc = source;
            } else {
                const int index = env_int_local("RPI_CAMERA_INDEX", 0);
                cap.open(index, cv::CAP_V4L2);
                source_desc = "index=" + std::to_string(index);
            }
            if (!cap.isOpened())
                throw std::runtime_error(
                    "open RPI camera failed source=" + source_desc +
                    " (run scripts/rpi_smoke.sh camera-probe and set RPI_CAMERA_SOURCE=/dev/videoX)");

            cap.set(cv::CAP_PROP_FRAME_WIDTH, request_w);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, request_h);
            cap.set(cv::CAP_PROP_FPS, request_fps);
        } else {
            request_w = kK230AiWidth;
            request_h = kK230AiHeight;
        }

        std::fprintf(stderr,
                     "rpi_camerad: source=%s capture=%dx%d@%d -> NV12 %ux%u shared_ring slots=%u",
                     replay ? "replay" : (synthetic ? "synthetic" : live_source_kind.c_str()),
                     request_w, request_h, request_fps,
                     kK230AiWidth, kK230AiHeight,
                     frame_ring.slot_count());
        if (replay)
            std::fprintf(stderr, " replay=%s frames=%u", replay_path.c_str(), replay_reader->frame_count());
        std::fprintf(stderr, "\n");

        uint64_t frame_id = 0;
        unsigned errors = 0;
        unsigned consecutive_read_errors = 0;
        unsigned last_frames = 0;
        unsigned last_errors = 0;
        timeval start {};
        timeval last {};
        gettimeofday(&start, nullptr);
        last = start;

        cv::Mat bgr;
        cv::Rect crop(0, 0, kK230AiWidth, kK230AiHeight);
        std::vector<uint8_t> nv12;
        while (!g_stop) {
            if (replay) {
                if (!replay_reader->read(nv12)) {
                    if (!replay_loop) break;
                    replay_reader->rewind();
                    if (!replay_reader->read(nv12)) break;
                }
                crop = cv::Rect(0, 0, kK230AiWidth, kK230AiHeight);
            } else if (synthetic) {
                fill_synthetic_nv12(nv12, frame_id);
                crop = cv::Rect(0, 0, kK230AiWidth, kK230AiHeight);
            } else {
                if (!cap.read(bgr) || bgr.empty()) {
                    ++errors;
                    ++consecutive_read_errors;
                    if (max_read_errors > 0 &&
                        consecutive_read_errors >= static_cast<unsigned>(max_read_errors)) {
                        std::fprintf(stderr,
                                     "\nrpi_camerad error: source read failed %u consecutive times after frames=%llu "
                                     "(run scripts/rpi_smoke.sh camera-probe and use a CAMERA_PROBE_NODE candidate=1 as RPI_CAMERA_SOURCE)\n",
                                     consecutive_read_errors,
                                     static_cast<unsigned long long>(frame_id));
                        break;
                    }
                    continue;
                }
                consecutive_read_errors = 0;
                bgr_to_nv12_512x256(bgr, nv12, &crop);
            }

            const unsigned slot = static_cast<unsigned>(frame_id % frame_ring.slot_count());
            uint8_t *dst = frame_ring.slot(slot);
            if (!dst) {
                ++errors;
                continue;
            }
            std::memcpy(dst, nv12.data(), nv12.size());

            K230RoadAiFrame msg;
            msg.frame_id = frame_id;
            msg.timestamp_ns = k230_now_ns();
            msg.slot = slot;
            msg.width = kK230AiWidth;
            msg.height = kK230AiHeight;
            msg.format = V4L2_PIX_FMT_NV12;
            msg.crop_x = static_cast<uint32_t>(crop.x);
            msg.crop_y = static_cast<uint32_t>(crop.y);
            msg.crop_width = static_cast<uint32_t>(crop.width);
            msg.crop_height = static_cast<uint32_t>(crop.height);
            if (!frame_pub.publish(&msg, sizeof(msg))) {
                ++errors;
            }

            ++frame_id;
            if (config.max_frames > 0 && frame_id >= config.max_frames) break;
            if ((synthetic || replay) && request_fps > 0)
                usleep(static_cast<useconds_t>(1000000 / request_fps));

            timeval now {};
            gettimeofday(&now, nullptr);
            const uint64_t duration = timeval_us(now) - timeval_us(last);
            if (duration >= 1000000ULL) {
                const unsigned frames_delta = static_cast<unsigned>(frame_id) - last_frames;
                const unsigned errors_delta = errors - last_errors;
                std::fprintf(stderr, "rpi_camerad: fps=%.2f frames=%llu errors=%u(+%u)          \r",
                             frames_delta * 1000000.0 / duration,
                             static_cast<unsigned long long>(frame_id), errors, errors_delta);
                std::fflush(stderr);
                last = now;
                last_frames = static_cast<unsigned>(frame_id);
                last_errors = errors;
            }
        }

        timeval end {};
        gettimeofday(&end, nullptr);
        const uint64_t total_us = timeval_us(end) - timeval_us(start);
        const double fps = total_us > 0 ? frame_id * 1000000.0 / total_us : 0.0;
        std::fprintf(stderr, "\nrpi_camerad done frames=%llu errors=%u fps=%.2f\n",
                     static_cast<unsigned long long>(frame_id), errors, fps);
        return frame_id > 0 && errors == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "rpi_camerad error: %s\n", e.what());
        return 1;
    }
}
