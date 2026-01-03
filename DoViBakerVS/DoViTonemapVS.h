#pragma once
#include "VapourSynth4++.hpp"
#include "DoViEetf.h"
#include <memory>
#include <cstdint>

using namespace vsxx4;

class DoViTonemapVS : public FilterBase {
public:
    DoViTonemapVS(void* = nullptr);

    const char* get_name(void*) noexcept override { return "DoViTonemap"; }

    void init(const ConstMap& in, const Map& out, const Core& core) override;
    ConstFrame get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*) override;
    ConstFrame get_frame(int n, const Core& core, const FrameContext& frame_context, void*) override;

private:
    template<int signalBitDepth>
    void applyTonemapRGB(Frame& dst, const ConstFrame& src) const;

    FilterNode m_clip;
    VSVideoInfo m_vi;
    int m_bitDepth;

    uint16_t m_targetMaxPq;
    uint16_t m_targetMinPq;
    mutable uint16_t m_masterMaxPq;
    mutable uint16_t m_masterMinPq;
    mutable float m_lumScale;
    mutable bool m_limitedInput;

    bool m_dynamicMasterMaxPq;
    bool m_dynamicMasterMinPq;
    bool m_dynamicLumScale;

    // EETF instances for different bit depths
    mutable std::unique_ptr<DoViEetf<10>> m_eetf10;
    mutable std::unique_ptr<DoViEetf<12>> m_eetf12;
    mutable std::unique_ptr<DoViEetf<14>> m_eetf14;
    mutable std::unique_ptr<DoViEetf<16>> m_eetf16;

    float m_kneeOffset;
    bool m_normalizeOutput;
};
