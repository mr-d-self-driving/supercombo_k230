#include "ncnn_output_contract.h"

#include <algorithm>

NcnnOutputSplitResult NcnnOutputContract::split_for_parser(const std::vector<float> &full_output,
                                                           std::vector<float> &parser_output,
                                                           std::vector<float> &recurrent_state)
{
    NcnnOutputSplitResult result;
    result.full_output_floats = full_output.size();
    if (recurrent_state.size() != static_cast<size_t>(kRecurrentFloats))
        recurrent_state.assign(kRecurrentFloats, 0.0f);

    if (full_output.size() >= static_cast<size_t>(kRecurrentFloats)) {
        std::copy(full_output.end() - kRecurrentFloats, full_output.end(), recurrent_state.begin());
        result.recurrent_updated = true;
        result.recurrent_floats = kRecurrentFloats;
    }

    if (full_output.size() == static_cast<size_t>(kPrunedVizOutputFloats)) {
        parser_output.assign(full_output.begin(), full_output.begin() + kParserFloats);
        result.pruned_viz = true;
    } else {
        parser_output = full_output;
    }

    result.parser_output_floats = parser_output.size();
    return result;
}
