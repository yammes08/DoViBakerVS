#pragma once
#include "VapourSynth4++.hpp"
#include <vector>
#include <string>
#include <tuple>
#include <cstdint>

using namespace vsxx4;

class DoViStatsFileLoaderVS : public FilterBase {
public:
    DoViStatsFileLoaderVS(void* = nullptr) : m_vi{}, m_currentScene(0), m_previousFrame(0), m_staticMaxPq(0), m_staticMaxCll(0) {}

    const char* get_name(void*) noexcept override { return "DoViStatsFileLoader"; }

    void init(const ConstMap& in, const Map& out, const Core& core) override;
    ConstFrame get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*) override;
    ConstFrame get_frame(int n, const Core& core, const FrameContext& frame_context, void*) override;

private:
    FilterNode m_clip;
    VSVideoInfo m_vi;
    std::vector<std::tuple<uint32_t, uint16_t, uint16_t, float>> m_sceneMaxSignal;
    mutable uint32_t m_currentScene;
    mutable uint32_t m_previousFrame;
    uint16_t m_staticMaxPq;
    uint16_t m_staticMaxCll;
};
