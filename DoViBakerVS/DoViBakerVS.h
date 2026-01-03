#pragma once
#include "VapourSynth4++.hpp"
#include "DoViProcessor.h"
#include <memory>
#include <array>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>

using namespace vsxx4;

// RAII wrapper for borrowing a processor from the pool
class DoViProcessorLease;

class DoViBakerVS : public FilterBase {
    friend class DoViProcessorLease;
public:
    DoViBakerVS(void* = nullptr);
    ~DoViBakerVS();

    const char* get_name(void*) noexcept override { return "DoViBaker"; }

    void init(const ConstMap& in, const Map& out, const Core& core) override;
    ConstFrame get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*) override;
    ConstFrame get_frame(int n, const Core& core, const FrameContext& frame_context, void*) override;

private:
    // Processor pool management
    DoViProcessor* acquireProcessor();
    void releaseProcessor(DoViProcessor* proc);

    // Upsampling helpers
    typedef uint16_t(*upscaler_t)(const uint16_t* srcSamples, int idx0);

    template<int vertLen, int nD>
    void upsampleVert(Frame& dst, const ConstFrame& src, int plane, const std::array<int, vertLen>& Dn0p,
                      const upscaler_t evenUpscaler, const upscaler_t oddUpscaler);

    template<int vertLen, int nD>
    void upsampleHorz(Frame& dst, const ConstFrame& src, int plane, const std::array<int, vertLen>& Dn0p,
                      const upscaler_t evenUpscaler, const upscaler_t oddUpscaler);

    // Processing helpers - take processor as parameter for thread safety
    template<bool blChromaSubsampling, bool elChromaSubsampling, bool quarterResolutionEl>
    void doAllQuickAndDirty(Frame& dst, const ConstFrame& blSrc, const ConstFrame& elSrc, DoViProcessor& proc) const;

    template<bool chromaSubsampling>
    void applyDovi(Frame& dst, const ConstFrame& blSrcY, const ConstFrame& blSrcUV,
                   const ConstFrame& elSrcY, const ConstFrame& elSrcUV, DoViProcessor& proc) const;

    void convert2rgb(Frame& dst, const ConstFrame& srcY, const ConstFrame& srcUV, DoViProcessor& proc) const;
    void applyTrim(Frame& dst, const ConstFrame& src, DoViProcessor& proc) const;

    Frame upscaleEl(const ConstFrame& src, const VSVideoInfo& dstVi, const Core& core);
    Frame upsampleChroma(const ConstFrame& src, const VSVideoInfo& dstVi, const Core& core);

    FilterNode m_blClip;
    FilterNode m_elClip;
    VSVideoInfo m_vi;
    VSVideoInfo m_blVi;
    VSVideoInfo m_elVi;

    // Processor pool for thread-safe parallel processing
    std::vector<std::unique_ptr<DoViProcessor>> m_processors;
    std::queue<DoViProcessor*> m_availableProcessors;
    mutable std::mutex m_poolMutex;
    std::condition_variable m_poolCV;
    const DoviRpuOpaqueList* m_sharedRpus = nullptr;
    bool m_ownsRpus = false;

    // Settings copied for pool processor creation
    int m_blContainerBits = 0;
    int m_elContainerBits = 0;
    int m_sourceProfile = 0;
    bool m_rgbProof = false;
    bool m_nlqProof = false;
    uint16_t m_trimPq = 0;
    float m_targetMaxNits = 100.0f;
    float m_targetMinNits = 0.0f;

    bool m_qnd;
    bool m_outYUV;
    bool m_blChromaSubSampled;
    bool m_elChromaSubSampled;
    bool m_quarterResolutionEl;
    bool m_hasEl;
};

// RAII wrapper for processor lease
class DoViProcessorLease {
public:
    DoViProcessorLease(DoViBakerVS& filter) : m_filter(filter), m_proc(filter.acquireProcessor()) {}
    ~DoViProcessorLease() { m_filter.releaseProcessor(m_proc); }
    DoViProcessor* operator->() { return m_proc; }
    DoViProcessor& operator*() { return *m_proc; }
private:
    DoViBakerVS& m_filter;
    DoViProcessor* m_proc;
};
