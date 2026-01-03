#pragma once
#include "VapourSynth4++.hpp"
#include "timecube.h"
#include <vector>
#include <memory>
#include <cstdint>

using namespace vsxx4;

class DoViCubesVS : public FilterBase {
public:
    DoViCubesVS(void* = nullptr) : m_vi{} {}

    const char* get_name(void*) noexcept override { return "DoViCubes"; }

    void init(const ConstMap& in, const Map& out, const Core& core) override;
    ConstFrame get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*) override;
    ConstFrame get_frame(int n, const Core& core, const FrameContext& frame_context, void*) override;

private:
    struct TimecubeLutFree {
        void operator()(timecube_lut* ptr) { timecube_lut_free(ptr); }
    };

    struct TimecubeFilterFree {
        void operator()(timecube_filter* ptr) { timecube_filter_free(ptr); }
    };

    void applyLut(Frame& dst, const ConstFrame& src, const timecube_filter* lut) const;

    FilterNode m_clip;
    VSVideoInfo m_vi;
    bool m_fullrange;
    std::vector<std::pair<uint16_t, std::unique_ptr<timecube_filter, TimecubeFilterFree>>> m_luts;
};
