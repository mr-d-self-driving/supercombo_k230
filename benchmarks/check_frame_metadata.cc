#include "k230_ipc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kNv12Fourcc = static_cast<uint32_t>('N') |
    (static_cast<uint32_t>('V') << 8) |
    (static_cast<uint32_t>('1') << 16) |
    (static_cast<uint32_t>('2') << 24);

int g_failures = 0;

bool env_is_present(const char *name)
{
    const char *value = std::getenv(name);
    return value && value[0] != '\0';
}

uint32_t env_u32(const char *name, uint32_t default_value)
{
    const char *value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return end == value ? default_value : static_cast<uint32_t>(parsed);
}

void check_eq(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) return;
    std::printf("FRAME_METADATA_CHECK field=%s actual=%u expected=%u result=FAIL\n",
                field, actual, expected);
    ++g_failures;
}

void check_positive_even(const char *field, uint32_t actual)
{
    if (actual > 0 && (actual % 2U) == 0) return;
    std::printf("FRAME_METADATA_CHECK field=%s actual=%u reason=not_positive_even result=FAIL\n",
                field, actual);
    ++g_failures;
}

std::string fourcc(uint32_t format)
{
    char text[5] = {
        static_cast<char>(format & 0xff),
        static_cast<char>((format >> 8) & 0xff),
        static_cast<char>((format >> 16) & 0xff),
        static_cast<char>((format >> 24) & 0xff),
        0,
    };
    for (int i = 0; i < 4; ++i) {
        if (text[i] < 32 || text[i] > 126) text[i] = '?';
    }
    return std::string(text);
}

} // namespace

int main()
{
    K230LatestChannel frame_sub;
    K230FrameRing frame_ring;
    if (!frame_sub.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), false)) {
        std::printf("FRAME_METADATA result=FAIL reason=open_roadAiFrame_failed\n");
        return 1;
    }
    if (!frame_ring.open(false, kK230AiWidth, kK230AiHeight, kK230FrameSlots)) {
        std::printf("FRAME_METADATA result=FAIL reason=open_frame_ring_failed\n");
        return 1;
    }

    K230RoadAiFrame msg;
    uint64_t seq = 0;
    if (!frame_sub.read(&msg, sizeof(msg), &seq)) {
        std::printf("FRAME_METADATA result=FAIL reason=read_roadAiFrame_failed\n");
        return 1;
    }

    check_eq("width", msg.width, env_u32("FRAME_EXPECT_WIDTH", kK230AiWidth));
    check_eq("height", msg.height, env_u32("FRAME_EXPECT_HEIGHT", kK230AiHeight));
    check_eq("format", msg.format, kNv12Fourcc);
    if (msg.slot >= frame_ring.slot_count()) {
        std::printf("FRAME_METADATA_CHECK field=slot actual=%u slots=%u result=FAIL\n",
                    msg.slot, frame_ring.slot_count());
        ++g_failures;
    }
    check_eq("ring_width", frame_ring.width(), kK230AiWidth);
    check_eq("ring_height", frame_ring.height(), kK230AiHeight);
    check_eq("ring_frame_bytes", frame_ring.frame_bytes(), kK230AiFrameBytes);
    check_positive_even("crop_width", msg.crop_width);
    check_positive_even("crop_height", msg.crop_height);

    if (env_is_present("FRAME_EXPECT_CROP_X")) check_eq("crop_x", msg.crop_x, env_u32("FRAME_EXPECT_CROP_X", 0));
    if (env_is_present("FRAME_EXPECT_CROP_Y")) check_eq("crop_y", msg.crop_y, env_u32("FRAME_EXPECT_CROP_Y", 0));
    if (env_is_present("FRAME_EXPECT_CROP_W")) check_eq("crop_width", msg.crop_width, env_u32("FRAME_EXPECT_CROP_W", 0));
    if (env_is_present("FRAME_EXPECT_CROP_H")) check_eq("crop_height", msg.crop_height, env_u32("FRAME_EXPECT_CROP_H", 0));

    const char *result = g_failures == 0 ? "PASS" : "FAIL";
    std::printf("FRAME_METADATA result=%s seq=%llu frame_id=%llu slot=%u width=%u height=%u "
                "format=%s crop=%u,%u,%u,%u ring=%ux%u bytes=%u slots=%u\n",
                result,
                static_cast<unsigned long long>(seq),
                static_cast<unsigned long long>(msg.frame_id),
                msg.slot,
                msg.width,
                msg.height,
                fourcc(msg.format).c_str(),
                msg.crop_x,
                msg.crop_y,
                msg.crop_width,
                msg.crop_height,
                frame_ring.width(),
                frame_ring.height(),
                frame_ring.frame_bytes(),
                frame_ring.slot_count());
    return g_failures == 0 ? 0 : 1;
}
