#include "app_config.h"
#include "k230_ipc.h"
#include "overlay_renderer.h"
#include "projection.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

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

bool env_enabled_local(const char *name, bool default_value)
{
    const char *value = std::getenv(name);
    if (!value) return default_value;
    return value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::string env_string_local(const char *name, const char *default_value)
{
    const char *value = std::getenv(name);
    return value && value[0] != '\0' ? std::string(value) : std::string(default_value);
}

void write_ppm_bgr(const std::string &path, const cv::Mat &bgr)
{
    if (path.empty()) return;
    std::ofstream file(path, std::ios::binary);
    if (!file) return;
    file << "P6\n" << bgr.cols << " " << bgr.rows << "\n255\n";
    for (int y = 0; y < bgr.rows; ++y) {
        const cv::Vec3b *row = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const char rgb[3] = {
                static_cast<char>(row[x][2]),
                static_cast<char>(row[x][1]),
                static_cast<char>(row[x][0]),
            };
            file.write(rgb, sizeof(rgb));
        }
    }
}

bool read_fb_virtual_size(int *width, int *height)
{
    std::ifstream file("/sys/class/graphics/fb0/virtual_size");
    char comma = 0;
    int w = 0;
    int h = 0;
    if (!(file >> w >> comma >> h) || comma != ',' || w <= 0 || h <= 0) return false;
    *width = w;
    *height = h;
    return true;
}

int read_fb_int(const char *path, int default_value)
{
    std::ifstream file(path);
    int value = default_value;
    file >> value;
    return value;
}

class FramebufferSink {
public:
    explicit FramebufferSink(const char *path)
        : path_(path ? path : "/dev/fb0")
    {
        if (!read_fb_virtual_size(&width_, &height_))
            throw std::runtime_error("read fb0 virtual_size failed");
        bpp_ = read_fb_int("/sys/class/graphics/fb0/bits_per_pixel", 16);
        stride_ = read_fb_int("/sys/class/graphics/fb0/stride", width_ * (bpp_ / 8));
        if (bpp_ != 16)
            throw std::runtime_error("only RGB565 framebuffer is supported");

        fd_ = open(path_.c_str(), O_RDWR);
        if (fd_ < 0)
            throw std::runtime_error("open framebuffer failed: " + path_);
        map_size_ = static_cast<size_t>(stride_) * height_;
        void *map = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            close();
            throw std::runtime_error("mmap framebuffer failed: " + path_);
        }
        map_ = static_cast<uint8_t *>(map);
        std::fprintf(stderr, "rpi_overlay: framebuffer=%s %dx%d bpp=%d stride=%d\n",
                     path_.c_str(), width_, height_, bpp_, stride_);
    }

    ~FramebufferSink()
    {
        close();
    }

    int width() const { return width_; }
    int height() const { return height_; }

    void present_bgr(const cv::Mat &bgr)
    {
        for (int y = 0; y < height_; ++y) {
            const cv::Vec3b *src = bgr.ptr<cv::Vec3b>(y);
            uint16_t *dst = reinterpret_cast<uint16_t *>(map_ + static_cast<size_t>(y) * stride_);
            for (int x = 0; x < width_; ++x) {
                const uint8_t b = src[x][0];
                const uint8_t g = src[x][1];
                const uint8_t r = src[x][2];
                dst[x] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }

private:
    void close()
    {
        if (map_) {
            munmap(map_, map_size_);
            map_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string path_;
    int fd_ = -1;
    uint8_t *map_ = nullptr;
    size_t map_size_ = 0;
    int width_ = 0;
    int height_ = 0;
    int bpp_ = 0;
    int stride_ = 0;
};

void blend_bgra_over_bgr(const cv::Mat &overlay, cv::Mat &bgr)
{
    for (int y = 0; y < bgr.rows; ++y) {
        const cv::Vec4b *src = overlay.ptr<cv::Vec4b>(y);
        cv::Vec3b *dst = bgr.ptr<cv::Vec3b>(y);
        for (int x = 0; x < bgr.cols; ++x) {
            const unsigned alpha = src[x][3];
            if (alpha == 0) continue;
            const unsigned inv = 255 - alpha;
            dst[x][0] = static_cast<uint8_t>((src[x][0] * alpha + dst[x][0] * inv) / 255);
            dst[x][1] = static_cast<uint8_t>((src[x][1] * alpha + dst[x][1] * inv) / 255);
            dst[x][2] = static_cast<uint8_t>((src[x][2] * alpha + dst[x][2] * inv) / 255);
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    (void)argc;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    try {
        AppConfig config = AppConfig::from_env_defaults(argv[0]);
        K230LatestChannel frame_sub;
        K230LatestChannel model_sub;
        K230FrameRing frame_ring;
        if (!frame_sub.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), true))
            throw std::runtime_error("open roadAiFrame ipc failed");
        if (!model_sub.open(kK230ModelStateTopic, sizeof(K230ModelState), true))
            throw std::runtime_error("open modelState ipc failed");
        while (!g_stop && !frame_ring.open(false)) {
            std::fprintf(stderr, "rpi_overlay: waiting for road ai frame ring\n");
            usleep(500000);
        }
        if (!frame_ring.valid()) return 1;

        const std::string display_mode = env_string_local("RPI_DISPLAY", "1");
        const bool display_none = display_mode == "0" || display_mode == "none";
        const bool display_fb = display_mode == "fb" || env_enabled_local("RPI_DISPLAY_FB", false);
        std::unique_ptr<FramebufferSink> fb;
        if (display_fb)
            fb.reset(new FramebufferSink(env_string_local("RPI_FB_PATH", "/dev/fb0").c_str()));
        const int out_w = display_fb ? fb->width() : static_cast<int>(env_unsigned("RPI_DISPLAY_W", 1024));
        const int out_h = display_fb ? fb->height() : static_cast<int>(env_unsigned("RPI_DISPLAY_H", 576));
        const unsigned overlay_fps_cap = env_unsigned("RPI_OVERLAY_FPS", 15);
        const uint64_t overlay_period_ns = overlay_fps_cap > 0
            ? 1000000000ULL / overlay_fps_cap
            : 0;
        const std::string dump_path = env_string_local("RPI_OVERLAY_DUMP", "");
        const bool render_output = !display_none || !dump_path.empty();
        const bool debug_hud = render_output &&
            (env_enabled_local("RPI_OVERLAY_DEBUG_HUD", false) || !dump_path.empty());
        if (!display_none && !display_fb)
            cv::namedWindow("supercombo_rpi", cv::WINDOW_NORMAL);
        if (!render_output)
            std::fprintf(stderr, "rpi_overlay: headless no-render mode\n");

        OverlayRenderer renderer;
        ProjectionState default_projection = make_projection_state(config.projection_mode,
                                                                   config.manual_roll,
                                                                   config.manual_pitch,
                                                                   config.manual_yaw);
        ProjectionState projection = default_projection;
        ParsedModelOutput parsed;
        bool have_model = false;
        uint64_t model_seq = 0;
        uint64_t frame_seq = 0;
        unsigned displayed = 0;
        unsigned errors = 0;
        uint64_t last_render_ns = 0;
        timeval start {};
        timeval last {};
        gettimeofday(&start, nullptr);
        last = start;

        cv::Mat nv12_mat;
        if (render_output) {
            nv12_mat = cv::Mat(static_cast<int>(frame_ring.height() * 3 / 2),
                               static_cast<int>(frame_ring.width()), CV_8UC1);
        }
        cv::Mat bgr;
        cv::Mat resized;
        cv::Mat overlay;
        if (render_output)
            overlay = cv::Mat(out_h, out_w, CV_8UC4);

        while (!g_stop) {
            K230ModelState state;
            uint64_t next_model_seq = model_seq;
            if (model_sub.read(&state, sizeof(state), &next_model_seq) && next_model_seq != model_seq) {
                model_seq = next_model_seq;
                have_model = state.valid != 0;
                parsed = k230_parsed_from_model_state(state);
                projection = k230_projection_from_model_state(state);
            }

            K230RoadAiFrame meta;
            if (!frame_sub.read_new(&frame_seq, &meta, sizeof(meta), 1000)) {
                std::fprintf(stderr, "rpi_overlay: waiting for roadAiFrame\n");
                continue;
            }
            if (meta.slot >= frame_ring.slot_count()) {
                ++errors;
                continue;
            }
            const uint8_t *src = frame_ring.slot(meta.slot);
            if (!src) {
                ++errors;
                continue;
            }
            if (meta.width != frame_ring.width() || meta.height != frame_ring.height()) {
                ++errors;
                continue;
            }

            const uint64_t now_ns = k230_now_ns();
            if (overlay_period_ns > 0 && last_render_ns != 0 &&
                now_ns - last_render_ns < overlay_period_ns) {
                continue;
            }
            last_render_ns = now_ns;

            if (render_output) {
                std::memcpy(nv12_mat.data, src, static_cast<size_t>(frame_ring.frame_bytes()));
                cv::cvtColor(nv12_mat, bgr, cv::COLOR_YUV2BGR_NV12);
                cv::resize(bgr, resized, cv::Size(out_w, out_h), 0.0, 0.0, cv::INTER_LINEAR);
                renderer.draw_mat(overlay, have_model ? parsed : ParsedModelOutput{},
                                  have_model ? projection : default_projection);
                blend_bgra_over_bgr(overlay, resized);
                if (debug_hud) {
                    cv::rectangle(resized, cv::Rect(0, 0, std::min(140, resized.cols), std::min(32, resized.rows)),
                                  cv::Scalar(30, 30, 30), cv::FILLED, cv::LINE_8);
                    cv::rectangle(resized, cv::Rect(6, 8, 18, 18), cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_8);
                    cv::rectangle(resized, cv::Rect(30, 8, 18, 18), cv::Scalar(0, 255, 0), cv::FILLED, cv::LINE_8);
                    cv::rectangle(resized, cv::Rect(54, 8, 18, 18), cv::Scalar(255, 0, 0), cv::FILLED, cv::LINE_8);
                    char text[64];
                    std::snprintf(text, sizeof(text), "f=%u m=%llu", displayed + 1,
                                  static_cast<unsigned long long>(model_seq));
                    cv::putText(resized, text, cv::Point(80, 22), cv::FONT_HERSHEY_SIMPLEX,
                                0.45, cv::Scalar(255, 255, 255), 1, cv::LINE_8);
                }
                if (!dump_path.empty())
                    write_ppm_bgr(dump_path, resized);

                if (display_fb) {
                    fb->present_bgr(resized);
                } else if (!display_none) {
                    cv::imshow("supercombo_rpi", resized);
                    const int key = cv::waitKey(1);
                    if (key == 27 || key == 'q') break;
                }
            }
            ++displayed;
            if (config.max_frames > 0 && displayed >= config.max_frames) break;

            timeval now {};
            gettimeofday(&now, nullptr);
            const uint64_t duration = timeval_us(now) - timeval_us(last);
            if (duration >= 1000000ULL) {
                std::fprintf(stderr,
                             "rpi_overlay: fps=%.2f frames=%u model_seq=%llu errors=%u display=%d          \r",
                             displayed * 1000000.0 / (timeval_us(now) - timeval_us(start)),
                             displayed,
                             static_cast<unsigned long long>(model_seq),
                             errors,
                             display_none ? 0 : 1);
                std::fflush(stderr);
                last = now;
            }
        }

        std::fprintf(stderr, "\nrpi_overlay done frames=%u errors=%u\n", displayed, errors);
        return displayed > 0 && errors == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "rpi_overlay error: %s\n", e.what());
        return 1;
    }
}
