#ifndef NCNN_OUTPUT_CONTRACT_H
#define NCNN_OUTPUT_CONTRACT_H

#include <cstddef>
#include <vector>

struct NcnnOutputSplitResult {
    bool pruned_viz = false;
    bool recurrent_updated = false;
    size_t full_output_floats = 0;
    size_t parser_output_floats = 0;
    size_t recurrent_floats = 0;
};

class NcnnOutputContract {
public:
    static constexpr int kParserFloats = 5755;
    static constexpr int kRecurrentFloats = 512;
    static constexpr int kPrunedVizOutputFloats = kParserFloats + kRecurrentFloats;

    static NcnnOutputSplitResult split_for_parser(const std::vector<float> &full_output,
                                                  std::vector<float> &parser_output,
                                                  std::vector<float> &recurrent_state);
};

#endif
