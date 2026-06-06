#include "k230_ipc.h"
#include "model_output.h"
#include "ncnn_output_contract.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kPlanMhpN = 5;
constexpr int kPlanStride = kTrajectorySize * 15 * 2 + 1;
constexpr int kLaneOffset = kPlanMhpN * kPlanStride;
constexpr int kLaneLineSize = 4 * kTrajectorySize * 2;
constexpr int kLaneProbOffset = kLaneOffset + kLaneLineSize * 2;
constexpr int kRoadEdgeOffset = kLaneProbOffset + 8;
constexpr int kRoadEdgeMeanSize = 2 * kTrajectorySize * 2;
constexpr int kBestPlan = 3;

int g_failures = 0;

void fail(const char *message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void expect_true(bool condition, const char *message)
{
    if (!condition) fail(message);
}

void expect_float(float actual, float expected, const char *message, float tolerance = 1e-6f)
{
    if (std::fabs(actual - expected) > tolerance) {
        std::fprintf(stderr, "FAIL: %s actual=%.9g expected=%.9g tolerance=%.3g\n",
                     message, actual, expected, tolerance);
        ++g_failures;
    }
}

float recurrent_value(unsigned generation, int index)
{
    return static_cast<float>(1000 * generation + index);
}

void fill_pruned_output(std::vector<float> &full_output, unsigned generation)
{
    full_output.assign(NcnnOutputContract::kPrunedVizOutputFloats, 0.0f);

    for (int plan = 0; plan < kPlanMhpN; ++plan)
        full_output[plan * kPlanStride + kPlanStride - 1] = plan == kBestPlan ? 4.0f : -4.0f;

    const int plan_base = kBestPlan * kPlanStride;
    const int plan_std_base = plan_base + kTrajectorySize * 15;
    for (int i = 0; i < kTrajectorySize; ++i) {
        const float x = ModelOutputParser::x_idx(i);
        const int mean_base = plan_base + i * 15;
        const int std_base = plan_std_base + i * 15;
        full_output[mean_base + 0] = x;
        full_output[mean_base + 1] = -0.05f * static_cast<float>(i);
        full_output[mean_base + 2] = 0.01f * static_cast<float>(i);
        full_output[mean_base + 9] = 0.001f * static_cast<float>(i);
        full_output[mean_base + 10] = -0.002f * static_cast<float>(i);
        full_output[mean_base + 11] = 0.003f * static_cast<float>(i);
        full_output[std_base + 0] = -2.0f;
        full_output[std_base + 1] = -2.0f;
        full_output[std_base + 2] = -2.0f;
    }

    for (int lane = 0; lane < 4; ++lane) {
        full_output[kLaneProbOffset + lane * 2 + 1] = lane == 2 ? 2.0f : -2.0f;
        const int std_base = kLaneOffset + kLaneLineSize + lane * kTrajectorySize * 2;
        full_output[std_base] = -1.0f;
        const int base = kLaneOffset + lane * kTrajectorySize * 2;
        for (int i = 0; i < kTrajectorySize; ++i) {
            full_output[base + i * 2 + 0] = static_cast<float>(lane - 2) + 0.01f * i;
            full_output[base + i * 2 + 1] = 0.02f * i;
        }
    }

    for (int edge = 0; edge < 2; ++edge) {
        const int std_base = kRoadEdgeOffset + kRoadEdgeMeanSize + edge * kTrajectorySize * 2;
        full_output[std_base] = -0.5f;
        const int base = kRoadEdgeOffset + edge * kTrajectorySize * 2;
        for (int i = 0; i < kTrajectorySize; ++i) {
            full_output[base + i * 2 + 0] = edge == 0 ? -3.0f : 3.0f;
            full_output[base + i * 2 + 1] = 0.03f * i;
        }
    }

    const int recurrent_base = NcnnOutputContract::kParserFloats;
    for (int i = 0; i < NcnnOutputContract::kRecurrentFloats; ++i)
        full_output[recurrent_base + i] = recurrent_value(generation, i);
}

void check_pruned_split_and_parser()
{
    std::vector<float> full_output;
    fill_pruned_output(full_output, 1);
    std::vector<float> parser_output;
    std::vector<float> recurrent_state(3, -1.0f);
    const NcnnOutputSplitResult split =
        NcnnOutputContract::split_for_parser(full_output, parser_output, recurrent_state);

    expect_true(split.pruned_viz, "pruned-viz output must be detected");
    expect_true(split.recurrent_updated, "pruned-viz output must update recurrent state");
    expect_true(split.full_output_floats == static_cast<size_t>(NcnnOutputContract::kPrunedVizOutputFloats),
                "full output size");
    expect_true(split.parser_output_floats == static_cast<size_t>(NcnnOutputContract::kParserFloats),
                "parser output size");
    expect_true(parser_output.size() == static_cast<size_t>(NcnnOutputContract::kParserFloats),
                "parser vector size");
    expect_true(recurrent_state.size() == static_cast<size_t>(NcnnOutputContract::kRecurrentFloats),
                "wrong-sized recurrent vector must normalize to 512");
    expect_float(parser_output.front(), full_output.front(), "parser first value");
    expect_float(parser_output.back(), full_output[NcnnOutputContract::kParserFloats - 1],
                 "parser last value before recurrent tail");
    expect_float(recurrent_state.front(), recurrent_value(1, 0), "recurrent first value");
    expect_float(recurrent_state.back(), recurrent_value(1, NcnnOutputContract::kRecurrentFloats - 1),
                 "recurrent last value");

    ParsedModelOutput parsed = ModelOutputParser::parse(parser_output);
    expect_true(parsed.valid, "parsed output validity");
    expect_true(parsed.plan.valid, "parsed plan validity");
    expect_true(parsed.plan.best_index == kBestPlan, "best plan index");
    expect_float(parsed.plan.points[10].x, ModelOutputParser::x_idx(10), "plan x");
    expect_float(parsed.plan.points[10].y, -0.5f, "plan y");
    expect_true(parsed.lanes[2].valid, "lane validity");
    expect_true(parsed.lanes[2].probability > 0.85f, "lane probability");
    expect_float(parsed.lanes[2].points[7].y, 0.07f, "lane y");
    expect_true(parsed.road_edges[1].valid, "road edge validity");
    expect_float(parsed.road_edges[1].points[5].y, 3.0f, "road edge y");
    expect_true(!parsed.leads.valid, "pruned-viz parser payload should not expose leads");
    expect_true(!parsed.has_pose, "pruned-viz parser payload should not expose pose");
    for (int i = 0; i < kDesireLen; ++i)
        expect_float(parsed.meta.desire_state[i], 0.0f, "pruned-viz parser payload should not expose meta");

    ProjectionState projection = make_projection_state(ProjectionMode::Legacy, 0.0f, 0.0f, 0.0f);
    OnlineCalibrator::Snapshot calibration;
    LateralTarget lateral;
    K230ModelState state;
    k230_fill_model_state(state, parsed, projection, calibration, lateral, 42, 123456, 55.0f);
    expect_true(state.valid == 1, "modelState valid");
    expect_true(state.best_plan == kBestPlan, "modelState best plan");
    expect_float(state.plan[10].x, ModelOutputParser::x_idx(10), "modelState plan x");
    expect_float(state.lanes[2][7].y, 0.07f, "modelState lane y");
    expect_float(state.road_edges[1][5].y, 3.0f, "modelState road edge y");
    expect_true(state.lead.valid == 0, "modelState lead absent");
    expect_true(state.pose.valid == 0, "modelState pose absent");
    for (int i = 0; i < kTrajectorySize; ++i) {
        expect_true(std::fabs(state.plan[i].x) < 900.0f &&
                        std::fabs(state.plan[i].y) < 900.0f &&
                        std::fabs(state.plan[i].z) < 900.0f,
                    "recurrent sentinel must not leak into plan");
        for (int lane = 0; lane < 4; ++lane) {
            expect_true(std::fabs(state.lanes[lane][i].y) < 900.0f &&
                            std::fabs(state.lanes[lane][i].z) < 900.0f,
                        "recurrent sentinel must not leak into lanes");
        }
        for (int edge = 0; edge < 2; ++edge) {
            expect_true(std::fabs(state.road_edges[edge][i].y) < 900.0f &&
                            std::fabs(state.road_edges[edge][i].z) < 900.0f,
                        "recurrent sentinel must not leak into road edges");
        }
    }
    for (int i = 0; i < kDesireLen; ++i)
        expect_float(state.desire_state[i], 0.0f, "modelState meta absent");

    std::vector<float> full_output_2;
    fill_pruned_output(full_output_2, 2);
    const NcnnOutputSplitResult split2 =
        NcnnOutputContract::split_for_parser(full_output_2, parser_output, recurrent_state);
    expect_true(split2.pruned_viz && split2.recurrent_updated, "second pruned split");
    expect_float(recurrent_state.front(), recurrent_value(2, 0), "recurrent carryover first update");
    expect_float(recurrent_state.back(), recurrent_value(2, NcnnOutputContract::kRecurrentFloats - 1),
                 "recurrent carryover last update");
}

void check_legacy_fallback()
{
    std::vector<float> recurrent_state(NcnnOutputContract::kRecurrentFloats, -7.0f);
    std::vector<float> parser_output;

    std::vector<float> custom_output(600, 0.0f);
    for (size_t i = 0; i < custom_output.size(); ++i)
        custom_output[i] = static_cast<float>(i) * 0.25f;
    const NcnnOutputSplitResult custom =
        NcnnOutputContract::split_for_parser(custom_output, parser_output, recurrent_state);
    expect_true(!custom.pruned_viz, "custom output is not pruned-viz");
    expect_true(custom.recurrent_updated, "legacy custom output updates recurrent state");
    expect_true(parser_output.size() == custom_output.size(), "custom output passes through");
    expect_float(parser_output[599], custom_output[599], "custom parser tail");
    expect_float(recurrent_state[0], custom_output[custom_output.size() - NcnnOutputContract::kRecurrentFloats],
                 "custom recurrent first");
    expect_float(recurrent_state.back(), custom_output.back(), "custom recurrent last");

    const float previous_recurrent_first = recurrent_state[0];
    const float previous_recurrent_last = recurrent_state.back();
    std::vector<float> short_output(300, 1.0f);
    const NcnnOutputSplitResult short_split =
        NcnnOutputContract::split_for_parser(short_output, parser_output, recurrent_state);
    expect_true(!short_split.pruned_viz, "short output is not pruned-viz");
    expect_true(!short_split.recurrent_updated, "short output does not update recurrent state");
    expect_true(parser_output.size() == short_output.size(), "short output passes through");
    expect_float(recurrent_state[0], previous_recurrent_first, "short output keeps recurrent first");
    expect_float(recurrent_state.back(), previous_recurrent_last, "short output keeps recurrent last");
}

} // namespace

int main()
{
    static_assert(NcnnOutputContract::kParserFloats == 5755, "parser payload changed");
    static_assert(NcnnOutputContract::kRecurrentFloats == 512, "recurrent size changed");
    static_assert(NcnnOutputContract::kPrunedVizOutputFloats == 6267, "pruned-viz output size changed");

    check_pruned_split_and_parser();
    check_legacy_fallback();

    const char *result = g_failures == 0 ? "PASS" : "FAIL";
    std::printf("NCNN_OUTPUT_CONTRACT result=%s full=%d parser=%d recurrent=%d "
                "best_plan=%d recurrent_first=%.1f recurrent_last=%.1f\n",
                result,
                NcnnOutputContract::kPrunedVizOutputFloats,
                NcnnOutputContract::kParserFloats,
                NcnnOutputContract::kRecurrentFloats,
                kBestPlan,
                recurrent_value(2, 0),
                recurrent_value(2, NcnnOutputContract::kRecurrentFloats - 1));
    return g_failures == 0 ? 0 : 1;
}
