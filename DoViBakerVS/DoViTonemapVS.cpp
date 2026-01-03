#include "DoViTonemapVS.h"
#include "DoViProcessor.h"
#include "VSHelper4.h"
#include <stdexcept>
#include <cmath>

DoViTonemapVS::DoViTonemapVS(void*)
    : m_vi{}
    , m_bitDepth(0)
    , m_targetMaxPq(0)
    , m_targetMinPq(0)
    , m_masterMaxPq(0)
    , m_masterMinPq(0)
    , m_lumScale(1.0f)
    , m_limitedInput(false)
    , m_dynamicMasterMaxPq(false)
    , m_dynamicMasterMinPq(false)
    , m_dynamicLumScale(false)
    , m_kneeOffset(0.75f)
    , m_normalizeOutput(false)
{
}

void DoViTonemapVS::init(const ConstMap& in, const Map& out, const Core& core)
{
    m_clip = in.get_prop<FilterNode>("clip");
    m_vi = m_clip.video_info();

    if (!vsh::isConstantVideoFormat(&m_vi))
        throw std::runtime_error("DoViTonemap: input must have constant format");

    if (m_vi.format.colorFamily != cfRGB)
        throw std::runtime_error("DoViTonemap: input must be RGB");

    m_bitDepth = m_vi.format.bitsPerSample;
    if (m_bitDepth != 10 && m_bitDepth != 12 && m_bitDepth != 14 && m_bitDepth != 16)
        throw std::runtime_error("DoViTonemap: bit depth must be 10, 12, 14, or 16");

    // Get parameters with defaults
    float targetMaxNits = static_cast<float>(in.get_prop<double>("targetMaxNits", map::default_val(1000.0)));
    float targetMinNits = static_cast<float>(in.get_prop<double>("targetMinNits", map::default_val(0.0)));
    float masterMaxNits = static_cast<float>(in.get_prop<double>("masterMaxNits", map::default_val(-1.0)));
    float masterMinNits = static_cast<float>(in.get_prop<double>("masterMinNits", map::default_val(-1.0)));
    float lumScale = static_cast<float>(in.get_prop<double>("lumScale", map::default_val(-1.0)));
    m_kneeOffset = static_cast<float>(in.get_prop<double>("kneeOffset", map::default_val(0.75)));
    m_normalizeOutput = in.get_prop<int64_t>("normalizeOutput", map::default_val(0LL)) != 0;

    m_targetMaxPq = DoViProcessor::nits2pq(targetMaxNits);
    m_targetMinPq = DoViProcessor::nits2pq(targetMinNits);
    m_masterMaxPq = DoViProcessor::nits2pq(masterMaxNits < 0 ? 10000.0f : masterMaxNits);
    m_masterMinPq = DoViProcessor::nits2pq(masterMinNits < 0 ? 0.0f : masterMinNits);
    m_lumScale = lumScale < 0 ? 1.0f : lumScale;
    m_limitedInput = false;

    m_dynamicMasterMaxPq = masterMaxNits < 0;
    m_dynamicMasterMinPq = masterMinNits < 0;
    m_dynamicLumScale = lumScale < 0;

    if (m_targetMinPq * 2 > m_targetMaxPq) {
        throw std::runtime_error("DoViTonemap: Value for 'targetMinNits' is too large to process");
    }
    if (m_masterMaxPq <= m_masterMinPq) {
        throw std::runtime_error("DoViTonemap: master capabilities given are invalid");
    }

    // Create appropriate EETF instance based on bit depth
    switch (m_bitDepth) {
        case 10:
            m_eetf10 = std::make_unique<DoViEetf<10>>(m_kneeOffset, m_normalizeOutput);
            m_eetf10->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
            break;
        case 12:
            m_eetf12 = std::make_unique<DoViEetf<12>>(m_kneeOffset, m_normalizeOutput);
            m_eetf12->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
            break;
        case 14:
            m_eetf14 = std::make_unique<DoViEetf<14>>(m_kneeOffset, m_normalizeOutput);
            m_eetf14->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
            break;
        case 16:
            m_eetf16 = std::make_unique<DoViEetf<16>>(m_kneeOffset, m_normalizeOutput);
            m_eetf16->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
            break;
    }

    create_video_filter(out, m_vi, fmParallel, simple_dep(m_clip, rpStrictSpatial), core);
}

ConstFrame DoViTonemapVS::get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*)
{
    frame_context.request_frame(n, m_clip);
    return nullptr;
}

ConstFrame DoViTonemapVS::get_frame(int n, const Core& core, const FrameContext& frame_context, void*)
{
    ConstFrame src = frame_context.get_frame(n, m_clip);
    Frame dst = core.new_video_frame(m_vi.format, m_vi.width, m_vi.height, src);

    uint16_t maxPq = m_masterMaxPq;
    uint16_t minPq = m_masterMinPq;
    float scale = m_lumScale;
    bool limited = m_limitedInput;

    // Check for _ColorRange property
    if (src.frame_props_ro().contains("_ColorRange")) {
        limited = src.frame_props_ro().get_prop<int64_t>("_ColorRange", map::default_val(0LL)) != 0;
    }

    // Output is always full range
    dst.frame_props_rw().set_prop("_ColorRange", static_cast<int64_t>(0));

    // Read dynamic parameters from frame properties if configured
    if (m_dynamicMasterMaxPq) {
        if (src.frame_props_ro().contains("_dovi_dynamic_max_pq")) {
            maxPq = static_cast<uint16_t>(src.frame_props_ro().get_prop<int64_t>("_dovi_dynamic_max_pq"));
        } else {
            throw std::runtime_error("DoViTonemap: Expected frame property '_dovi_dynamic_max_pq' not available. Set 'masterMaxNits' explicitly.");
        }
    }

    if (m_dynamicMasterMinPq) {
        if (src.frame_props_ro().contains("_dovi_dynamic_min_pq")) {
            minPq = static_cast<uint16_t>(src.frame_props_ro().get_prop<int64_t>("_dovi_dynamic_min_pq"));
        } else {
            throw std::runtime_error("DoViTonemap: Expected frame property '_dovi_dynamic_min_pq' not available. Set 'masterMinNits' explicitly.");
        }
    }

    if (m_dynamicLumScale) {
        if (src.frame_props_ro().contains("_dovi_dynamic_luminosity_scale")) {
            scale = static_cast<float>(src.frame_props_ro().get_prop<double>("_dovi_dynamic_luminosity_scale"));
        } else {
            throw std::runtime_error("DoViTonemap: Expected frame property '_dovi_dynamic_luminosity_scale' not available. Set 'lumScale' explicitly.");
        }
    }

    // Regenerate EETF if parameters changed
    if (maxPq != m_masterMaxPq || minPq != m_masterMinPq || std::abs(scale - m_lumScale) > 0.001f || limited != m_limitedInput) {
        m_masterMaxPq = maxPq;
        m_masterMinPq = minPq;
        m_lumScale = scale;
        m_limitedInput = limited;

        switch (m_bitDepth) {
            case 10:
                m_eetf10->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
                break;
            case 12:
                m_eetf12->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
                break;
            case 14:
                m_eetf14->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
                break;
            case 16:
                m_eetf16->generateEETF(m_targetMaxPq, m_targetMinPq, m_masterMaxPq, m_masterMinPq, m_lumScale, m_limitedInput);
                break;
        }
    }

    // Apply tonemap based on bit depth
    switch (m_bitDepth) {
        case 10: applyTonemapRGB<10>(dst, src); break;
        case 12: applyTonemapRGB<12>(dst, src); break;
        case 14: applyTonemapRGB<14>(dst, src); break;
        case 16: applyTonemapRGB<16>(dst, src); break;
    }

    return dst;
}

template<int signalBitDepth>
void DoViTonemapVS::applyTonemapRGB(Frame& dst, const ConstFrame& src) const
{
    const DoViEetf<signalBitDepth>* eetf = nullptr;
    if constexpr (signalBitDepth == 10) eetf = m_eetf10.get();
    else if constexpr (signalBitDepth == 12) eetf = m_eetf12.get();
    else if constexpr (signalBitDepth == 14) eetf = m_eetf14.get();
    else if constexpr (signalBitDepth == 16) eetf = m_eetf16.get();

    const int height = src.height(0);
    const int width = m_vi.width;

    for (int p = 0; p < 3; ++p) {
        const uint16_t* srcP = reinterpret_cast<const uint16_t*>(src.read_ptr(p));
        uint16_t* dstP = reinterpret_cast<uint16_t*>(dst.write_ptr(p));
        const ptrdiff_t srcStride = src.stride(p) / sizeof(uint16_t);
        const ptrdiff_t dstStride = dst.stride(p) / sizeof(uint16_t);

        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                dstP[w] = eetf->applyEETF(srcP[w]);
            }
            srcP += srcStride;
            dstP += dstStride;
        }
    }
}

// Explicit template instantiations
template void DoViTonemapVS::applyTonemapRGB<10>(Frame& dst, const ConstFrame& src) const;
template void DoViTonemapVS::applyTonemapRGB<12>(Frame& dst, const ConstFrame& src) const;
template void DoViTonemapVS::applyTonemapRGB<14>(Frame& dst, const ConstFrame& src) const;
template void DoViTonemapVS::applyTonemapRGB<16>(Frame& dst, const ConstFrame& src) const;
