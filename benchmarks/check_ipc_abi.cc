#include "k230_ipc.h"

#include <sys/mman.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void fail(const char *message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

bool near_float(float actual, float expected, float tolerance = 1e-6f)
{
    return std::fabs(actual - expected) <= tolerance;
}

void expect_true(bool condition, const char *message)
{
    if (!condition) fail(message);
}

void expect_float(float actual, float expected, const char *message)
{
    if (!near_float(actual, expected)) {
        std::fprintf(stderr, "FAIL: %s actual=%.9g expected=%.9g\n", message, actual, expected);
        ++g_failures;
    }
}

void cleanup_ipc()
{
    shm_unlink(kK230RoadAiFrameRing);
    shm_unlink(kK230RoadAiFrameTopic);
    shm_unlink(kK230ModelStateTopic);
}

uint8_t pattern_byte(unsigned slot, unsigned offset)
{
    return static_cast<uint8_t>((slot * 41 + offset * 7 + 13) & 0xff);
}

void fill_parsed_output(ParsedModelOutput &parsed)
{
    parsed.valid = true;
    parsed.plan.valid = true;
    parsed.plan.best_index = 2;
    parsed.plan.probability = 0.73f;
    for (int i = 0; i < kTrajectorySize; ++i) {
        const float t = static_cast<float>(i);
        parsed.plan.points[i] = {1.0f + t * 2.0f, -0.2f * t, 0.01f * t};
        parsed.plan.position_stds[i] = {0.1f + t * 0.01f, 0.2f + t * 0.01f, 0.3f + t * 0.01f};
        parsed.plan.orientations[i] = {0.01f * t, -0.02f * t, 0.03f * t};
        for (int lane = 0; lane < 4; ++lane) {
            parsed.lanes[lane].valid = true;
            parsed.lanes[lane].probability = 0.2f + lane * 0.1f;
            parsed.lanes[lane].std = 0.05f + lane * 0.01f;
            parsed.lanes[lane].points[i] = {
                1.5f + t,
                static_cast<float>(lane - 1.5f) + 0.01f * t,
                0.02f * t,
            };
        }
        for (int edge = 0; edge < 2; ++edge) {
            parsed.road_edges[edge].valid = true;
            parsed.road_edges[edge].std = 0.2f + edge * 0.1f;
            parsed.road_edges[edge].points[i] = {
                2.0f + t,
                edge == 0 ? -3.0f : 3.0f,
                0.03f * t,
            };
        }
    }
    for (int i = 0; i < kDesireLen; ++i)
        parsed.meta.desire_state[i] = 0.01f * static_cast<float>(i + 1);

    parsed.leads.valid = true;
    parsed.leads.global_probabilities[0] = 0.82f;
    parsed.leads.predictions[0].probabilities[0] = 0.82f;
    parsed.leads.predictions[0].points[0] = {24.0f, -0.7f, 1.2f, -0.05f};

    parsed.has_pose = true;
    for (int i = 0; i < 3; ++i) {
        parsed.pose.trans[i] = 10.0f + i;
        parsed.pose.rot[i] = 0.01f * static_cast<float>(i + 1);
        parsed.pose.trans_std[i] = 0.03f * static_cast<float>(i + 1);
        parsed.pose.rot_std[i] = 0.004f * static_cast<float>(i + 1);
    }
}

LateralTarget make_lateral_target()
{
    LateralTarget lateral;
    lateral.valid = true;
    lateral.mpc_solution_valid = true;
    lateral.lookahead_x = 17.0f;
    lateral.target_y = -0.4f;
    lateral.heading = 0.02f;
    lateral.curvature = 0.001f;
    lateral.output_scale = 1.0f;
    for (int i = 0; i < kLateralControlN; ++i) {
        const float t = static_cast<float>(i);
        lateral.psis[i] = 0.01f * t;
        lateral.curvatures[i] = 0.001f * t;
        lateral.curvature_rates[i] = 0.0001f * t;
        lateral.d_path_points[i] = -0.1f * t;
    }
    return lateral;
}

void check_frame_ring()
{
    K230FrameRing writer;
    K230FrameRing reader;
    expect_true(writer.open(true, kK230AiWidth, kK230AiHeight, kK230FrameSlots),
                "create frame ring");
    expect_true(reader.open(false), "open frame ring reader");
    expect_true(writer.slot_count() == kK230FrameSlots, "frame ring slot count");
    expect_true(writer.width() == kK230AiWidth && writer.height() == kK230AiHeight,
                "frame ring dimensions");
    expect_true(writer.frame_bytes() == kK230AiFrameBytes, "frame ring frame bytes");

    for (unsigned slot = 0; slot < writer.slot_count(); ++slot) {
        uint8_t *dst = writer.slot(slot);
        expect_true(dst != nullptr, "frame ring writer slot");
        if (!dst) continue;
        for (unsigned i = 0; i < writer.frame_bytes(); ++i)
            dst[i] = pattern_byte(slot, i);
    }
    for (unsigned slot = 0; slot < reader.slot_count(); ++slot) {
        const uint8_t *src = reader.slot(slot);
        expect_true(src != nullptr, "frame ring reader slot");
        if (!src) continue;
        for (unsigned i = 0; i < reader.frame_bytes(); i += 4099) {
            if (src[i] != pattern_byte(slot, i)) {
                fail("frame ring slot stride/content mismatch");
                break;
            }
        }
    }
}

void check_latest_frame_channel()
{
    K230LatestChannel writer;
    K230LatestChannel reader;
    expect_true(writer.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), true),
                "create roadAiFrame channel");
    expect_true(reader.open(kK230RoadAiFrameTopic, sizeof(K230RoadAiFrame), false),
                "open roadAiFrame reader");

    K230RoadAiFrame first;
    first.frame_id = 10;
    first.slot = 2;
    first.width = kK230AiWidth;
    first.height = kK230AiHeight;
    first.format = 0x3231564e; // NV12
    expect_true(writer.publish(&first, sizeof(first)), "publish first roadAiFrame");

    K230RoadAiFrame second = first;
    second.frame_id = 11;
    second.slot = 3;
    expect_true(writer.publish(&second, sizeof(second)), "publish second roadAiFrame");

    uint64_t seq = 0;
    K230RoadAiFrame out;
    expect_true(reader.read_new(&seq, &out, sizeof(out), 0), "read latest roadAiFrame");
    expect_true(out.frame_id == 11 && out.slot == 3, "latest roadAiFrame content");
    expect_true(seq != 0 && (seq & 1ULL) == 0, "latest channel even sequence");
    expect_true(!reader.read_new(&seq, &out, sizeof(out), 0), "read_new conflates unchanged seq");
}

void check_model_state_round_trip()
{
    ParsedModelOutput parsed;
    fill_parsed_output(parsed);
    ProjectionState projection = make_projection_state(ProjectionMode::Openpilot, 0.01f, -0.02f, 0.03f);
    OnlineCalibrator::Snapshot calibration;
    calibration.status = CalibrationStatus::Calibrated;
    calibration.valid_blocks = 7;
    calibration.rpy[0] = projection.roll;
    calibration.rpy[1] = projection.pitch;
    calibration.rpy[2] = projection.yaw;
    calibration.spread[0] = 0.001f;
    calibration.spread[1] = 0.002f;
    calibration.spread[2] = 0.003f;
    LateralTarget lateral = make_lateral_target();

    K230ModelState state;
    k230_fill_model_state(state, parsed, projection, calibration, lateral, 1234, 5678, 42.5f);

    K230LatestChannel writer;
    K230LatestChannel reader;
    expect_true(writer.open(kK230ModelStateTopic, sizeof(K230ModelState), true),
                "create modelState channel");
    expect_true(reader.open(kK230ModelStateTopic, sizeof(K230ModelState), false),
                "open modelState reader");
    expect_true(writer.publish(&state, sizeof(state)), "publish modelState");

    K230ModelState readback;
    uint64_t seq = 0;
    expect_true(reader.read_new(&seq, &readback, sizeof(readback), 0), "read modelState");
    expect_true(readback.frame_id == 1234, "modelState frame id");
    expect_true(readback.capture_timestamp_ns == 5678, "modelState capture timestamp");
    expect_float(readback.model_execution_ms, 42.5f, "modelState execution time");
    expect_true(readback.projection_mode == static_cast<uint32_t>(ProjectionMode::Openpilot),
                "modelState projection mode");

    ParsedModelOutput parsed_back = k230_parsed_from_model_state(readback);
    expect_true(parsed_back.valid && parsed_back.plan.valid, "parsed modelState validity");
    expect_true(parsed_back.plan.best_index == parsed.plan.best_index, "plan best index");
    expect_float(parsed_back.plan.probability, parsed.plan.probability, "plan probability");
    expect_float(parsed_back.plan.points[10].x, parsed.plan.points[10].x, "plan x");
    expect_float(parsed_back.plan.points[10].y, parsed.plan.points[10].y, "plan y");
    expect_float(parsed_back.lanes[2].probability, parsed.lanes[2].probability, "lane probability");
    expect_float(parsed_back.lanes[2].points[7].y, parsed.lanes[2].points[7].y, "lane y");
    expect_float(parsed_back.road_edges[1].points[5].y, parsed.road_edges[1].points[5].y, "road edge y");
    expect_float(parsed_back.meta.desire_state[3], parsed.meta.desire_state[3], "desire state");
    expect_true(parsed_back.has_pose, "pose valid");
    expect_float(parsed_back.pose.trans[0], parsed.pose.trans[0], "pose trans");

    ParsedLeadPoint lead;
    float lead_prob = 0.0f;
    expect_true(parsed_back.leads.primary(0, 0.0f, &lead, &lead_prob), "lead round trip valid");
    expect_float(lead_prob, 0.82f, "lead probability");
    expect_float(lead.x, 24.0f, "lead x");

    ProjectionState projection_back = k230_projection_from_model_state(readback);
    expect_true(projection_back.mode == ProjectionMode::Openpilot, "projection round trip mode");
    expect_float(projection_back.roll, projection.roll, "projection roll");
    expect_float(projection_back.pitch, projection.pitch, "projection pitch");
    expect_float(projection_back.yaw, projection.yaw, "projection yaw");

    expect_true(readback.calibration.status == static_cast<uint32_t>(CalibrationStatus::Calibrated),
                "calibration status");
    expect_true(readback.calibration.valid_blocks == 7, "calibration valid blocks");
    expect_float(readback.lateral_target.lookahead_x, lateral.lookahead_x, "lateral target lookahead");
    expect_float(readback.lateral_plan.psis[6], lateral.psis[6], "lateral plan psi");
    expect_float(readback.lateral_plan.d_path_points[6], lateral.d_path_points[6], "lateral plan d path");
}

} // namespace

int main()
{
    static_assert(sizeof(K230IpcHeader) == 40, "K230IpcHeader size changed");
    static_assert(sizeof(K230FrameRingHeader) == 32, "K230FrameRingHeader size changed");
    static_assert(sizeof(K230RoadAiFrame) == 48, "K230RoadAiFrame size changed");
    static_assert(kK230FrameSlots == 4, "frame slot count changed");
    static_assert(kK230AiWidth == 512 && kK230AiHeight == 256, "AI frame dimensions changed");
    static_assert(kK230AiFrameBytes == 512 * 256 * 3 / 2, "AI frame bytes changed");

    cleanup_ipc();
    check_frame_ring();
    check_latest_frame_channel();
    check_model_state_round_trip();

    const char *result = g_failures == 0 ? "PASS" : "FAIL";
    std::printf("IPC_ABI result=%s header=%zu ring_header=%zu road_ai_frame=%zu "
                "model_state=%zu frame_bytes=%u slots=%u\n",
                result,
                sizeof(K230IpcHeader),
                sizeof(K230FrameRingHeader),
                sizeof(K230RoadAiFrame),
                sizeof(K230ModelState),
                kK230AiFrameBytes,
                kK230FrameSlots);
    cleanup_ipc();
    return g_failures == 0 ? 0 : 1;
}
