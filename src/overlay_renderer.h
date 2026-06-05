#ifndef OVERLAY_RENDERER_H
#define OVERLAY_RENDERER_H

#include "model_output.h"
#include "projection.h"

#include <cstdint>

namespace cv {
class Mat;
}

class OverlayRenderer {
public:
    OverlayRenderer() = default;

    void draw_argb(uint8_t *argb, int width, int height, int stride,
                   const ParsedModelOutput &output,
                   const ProjectionState &projection) const;
    void draw_mat(cv::Mat &frame, const ParsedModelOutput &output,
                  const ProjectionState &projection) const;
};

#endif
