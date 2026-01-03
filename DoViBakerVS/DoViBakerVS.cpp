#include "DoViBakerVS.h"
#include "VSHelper4.h"
#include <stdexcept>
#include <algorithm>
#include <cstring>
#include <thread>

// Get pool size based on hardware concurrency
static int getPoolSize() {
    int hwThreads = static_cast<int>(std::thread::hardware_concurrency());
    // Minimum 4, maximum 32, default to 8 if detection fails
    if (hwThreads <= 0) hwThreads = 8;
    return std::min(32, std::max(4, hwThreads));
}

DoViBakerVS::DoViBakerVS(void*)
    : m_vi{}
    , m_blVi{}
    , m_elVi{}
    , m_qnd(false)
    , m_outYUV(false)
    , m_blChromaSubSampled(false)
    , m_elChromaSubSampled(false)
    , m_quarterResolutionEl(false)
    , m_hasEl(false)
{
}

DoViBakerVS::~DoViBakerVS()
{
    // Clear processors before freeing shared RPU data
    m_processors.clear();

    // Free shared RPU data if we own it
    if (m_ownsRpus && m_sharedRpus) {
        dovi_rpu_list_free(m_sharedRpus);
    }
}

DoViProcessor* DoViBakerVS::acquireProcessor()
{
    std::unique_lock<std::mutex> lock(m_poolMutex);
    m_poolCV.wait(lock, [this] { return !m_availableProcessors.empty(); });
    DoViProcessor* proc = m_availableProcessors.front();
    m_availableProcessors.pop();
    return proc;
}

void DoViBakerVS::releaseProcessor(DoViProcessor* proc)
{
    std::lock_guard<std::mutex> lock(m_poolMutex);
    m_availableProcessors.push(proc);
    m_poolCV.notify_one();
}

void DoViBakerVS::init(const ConstMap& in, const Map& out, const Core& core)
{
    // Get base layer clip (required)
    m_blClip = in.get_prop<FilterNode>("bl");
    m_blVi = m_blClip.video_info();

    if (!vsh::isConstantVideoFormat(&m_blVi))
        throw std::runtime_error("DoViBaker: base layer must have constant format");

    if (m_blVi.format.colorFamily != cfYUV)
        throw std::runtime_error("DoViBaker: base layer must be YUV");

    // Get enhancement layer clip (optional)
    m_hasEl = in.contains("el");
    if (m_hasEl) {
        m_elClip = in.get_prop<FilterNode>("el");
        m_elVi = m_elClip.video_info();

        if (!vsh::isConstantVideoFormat(&m_elVi))
            throw std::runtime_error("DoViBaker: enhancement layer must have constant format");

        if (m_elVi.format.colorFamily != cfYUV)
            throw std::runtime_error("DoViBaker: enhancement layer must be YUV");

        if (m_blVi.numFrames != m_elVi.numFrames) {
            throw std::runtime_error("DoViBaker: base layer and enhancement layer must have the same number of frames");
        }

        // Check if EL is quarter resolution
        m_quarterResolutionEl = (m_elVi.width == m_blVi.width / 2) && (m_elVi.height == m_blVi.height / 2);
    }

    // Determine chroma subsampling
    m_blChromaSubSampled = (m_blVi.format.subSamplingW == 1 && m_blVi.format.subSamplingH == 1);
    if (m_hasEl) {
        m_elChromaSubSampled = (m_elVi.format.subSamplingW == 1 && m_elVi.format.subSamplingH == 1);
    } else {
        m_elChromaSubSampled = m_blChromaSubSampled;
    }

    // Get RPU path (optional - may be integrated in frame properties)
    const char* rpuPath = nullptr;
    if (in.contains("rpu")) {
        rpuPath = in.get_prop<const char*>("rpu");
    }

    // Get parameters and save for pool processor creation
    m_trimPq = static_cast<uint16_t>(in.get_prop<int64_t>("trimPq", map::default_val(0LL)));
    m_targetMaxNits = static_cast<float>(in.get_prop<double>("targetMaxNits", map::default_val(100.0)));
    m_targetMinNits = static_cast<float>(in.get_prop<double>("targetMinNits", map::default_val(0.0)));
    m_qnd = in.get_prop<int64_t>("qnd", map::default_val(0LL)) != 0;
    m_rgbProof = in.get_prop<int64_t>("rgbProof", map::default_val(0LL)) != 0;
    m_nlqProof = in.get_prop<int64_t>("nlqProof", map::default_val(0LL)) != 0;
    m_outYUV = in.get_prop<int64_t>("outYUV", map::default_val(0LL)) != 0;
    m_sourceProfile = static_cast<int>(in.get_prop<int64_t>("sourceProfile", map::default_val(0LL)));

    // Validate sourceProfile (must be 0, 7, or 8)
    if (m_sourceProfile != 0 && m_sourceProfile != 7 && m_sourceProfile != 8) {
        throw std::runtime_error("DoViBaker: sourceProfile must be 0 (auto), 7, or 8");
    }

    // Validate outYUV restrictions (matching quietvoid fork behavior)
    if (m_outYUV) {
        if (m_hasEl && m_blChromaSubSampled != m_elChromaSubSampled) {
            throw std::runtime_error("DoViBaker: Both BL and EL must have the same chroma subsampling when outYUV=true");
        }
        if (m_qnd) {
            throw std::runtime_error("DoViBaker: qnd mode cannot be used when outYUV=true");
        }
        if (m_rgbProof) {
            throw std::runtime_error("DoViBaker: rgbProof cannot be used when outYUV=true");
        }
    }

    // Save container bits for pool creation
    m_blContainerBits = m_blVi.format.bitsPerSample;
    m_elContainerBits = m_hasEl ? m_elVi.format.bitsPerSample : 0;

    // Create the first processor to validate settings and parse RPU file
    auto firstProc = std::make_unique<DoViProcessor>(rpuPath, nullptr, m_blContainerBits, m_elContainerBits, m_sourceProfile);
    if (!firstProc->wasCreationSuccessful()) {
        throw std::runtime_error("DoViBaker: Cannot create DoViProcessor");
    }

    // Validate clip lengths match RPU if not integrated
    if (!firstProc->isIntegratedRpu() && m_blVi.numFrames != firstProc->getClipLength()) {
        throw std::runtime_error("DoViBaker: Clip length does not match length indicated by RPU file");
    }

    // Get the shared RPU data from the first processor
    m_sharedRpus = firstProc->getRpuList();
    m_ownsRpus = false;  // First processor owns it, we just share

    // Configure the first processor
    firstProc->setRgbProof(m_rgbProof);
    firstProc->setNlqProof(m_nlqProof);
    firstProc->setTrim(m_trimPq, m_targetMinNits, m_targetMaxNits);

    // Add first processor to pool
    m_availableProcessors.push(firstProc.get());
    m_processors.push_back(std::move(firstProc));

    // Create additional processors sharing the same RPU data
    const int poolSize = getPoolSize();
    for (int i = 1; i < poolSize; ++i) {
        auto proc = std::make_unique<DoViProcessor>(m_sharedRpus, m_blContainerBits, m_elContainerBits, m_sourceProfile);
        if (!proc->wasCreationSuccessful()) {
            throw std::runtime_error("DoViBaker: Cannot create pool DoViProcessor");
        }
        proc->setRgbProof(m_rgbProof);
        proc->setNlqProof(m_nlqProof);
        proc->setTrim(m_trimPq, m_targetMinNits, m_targetMaxNits);
        m_availableProcessors.push(proc.get());
        m_processors.push_back(std::move(proc));
    }

    // Set output format based on outYUV parameter
    m_vi = m_blVi;
    if (m_outYUV) {
        // YUV output - preserve input chroma subsampling
        m_vi.format = core.query_video_format(cfYUV, stInteger, 16,
            m_blChromaSubSampled ? 1 : 0, m_blChromaSubSampled ? 1 : 0);
    } else {
        // RGB48 output (16-bit planar RGB)
        m_vi.format = core.query_video_format(cfRGB, stInteger, 16, 0, 0);
    }

    // Register filter - now safe to use fmParallel with processor pool
    if (m_hasEl) {
        create_video_filter(out, m_vi, fmParallel,
            make_deps().add_dep(m_blClip, rpStrictSpatial).add_dep(m_elClip, rpStrictSpatial), core);
    } else {
        create_video_filter(out, m_vi, fmParallel, simple_dep(m_blClip, rpStrictSpatial), core);
    }
}

ConstFrame DoViBakerVS::get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*)
{
    frame_context.request_frame(n, m_blClip);
    if (m_hasEl) {
        frame_context.request_frame(n, m_elClip);
    }
    return nullptr;
}

ConstFrame DoViBakerVS::get_frame(int n, const Core& core, const FrameContext& frame_context, void*)
{
    ConstFrame blSrc = frame_context.get_frame(n, m_blClip);
    ConstFrame elSrc = m_hasEl ? frame_context.get_frame(n, m_elClip) : blSrc;

    Frame dst = core.new_video_frame(m_vi.format, m_vi.width, m_vi.height, blSrc);

    // Acquire a processor from the pool (RAII - automatically released when lease goes out of scope)
    DoViProcessorLease proc(*const_cast<DoViBakerVS*>(this));

    // Extract RPU from frame properties if integrated
    const uint8_t* rpubuf = nullptr;
    size_t rpusize = 0;

    if (proc->isIntegratedRpu()) {
        if (blSrc.frame_props_ro().contains("DolbyVisionRPU")) {
            // Get binary data from frame property
            auto props = blSrc.frame_props_ro();
            int numElements = props.num_elements("DolbyVisionRPU");
            if (numElements > 0) {
                int error = 0;
                rpubuf = reinterpret_cast<const uint8_t*>(props.get_prop<const char*>("DolbyVisionRPU"));
                rpusize = get_vsapi()->mapGetDataSize(props.get(), "DolbyVisionRPU", 0, &error);
            }
        } else if (m_hasEl && elSrc.frame_props_ro().contains("DolbyVisionRPU")) {
            auto props = elSrc.frame_props_ro();
            int numElements = props.num_elements("DolbyVisionRPU");
            if (numElements > 0) {
                int error = 0;
                rpubuf = reinterpret_cast<const uint8_t*>(props.get_prop<const char*>("DolbyVisionRPU"));
                rpusize = get_vsapi()->mapGetDataSize(props.get(), "DolbyVisionRPU", 0, &error);
            }
        }
    }

    // Initialize DoViProcessor for this frame
    bool doviInitialized = proc->intializeFrame(n, nullptr, rpubuf, rpusize);
    if (!doviInitialized) {
        return dst;
    }

    // Set frame properties
    if (m_outYUV) {
        // YUV output - set matrix to BT.2020 NCL (9)
        dst.frame_props_rw().set_prop("_Matrix", static_cast<int64_t>(9));
        dst.frame_props_rw().set_prop("_ColorRange", static_cast<int64_t>(1));  // Limited range
        dst.frame_props_rw().set_prop("_Primaries", static_cast<int64_t>(9));   // BT.2020
        dst.frame_props_rw().set_prop("_Transfer", static_cast<int64_t>(16));   // PQ (SMPTE ST 2084)
    } else {
        // RGB output
        dst.frame_props_rw().set_prop("_Matrix", static_cast<int64_t>(0));
        dst.frame_props_rw().set_prop("_ColorRange", static_cast<int64_t>(proc->isLimitedRangeOutput() ? 1 : 0));
    }
    dst.frame_props_rw().set_prop("_SceneChangePrev", static_cast<int64_t>(proc->isSceneChange() ? 1 : 0));
    dst.frame_props_rw().set_prop("_dovi_dynamic_min_pq", static_cast<int64_t>(proc->getDynamicMinPq()));
    dst.frame_props_rw().set_prop("_dovi_dynamic_max_pq", static_cast<int64_t>(proc->getDynamicMaxPq()));
    dst.frame_props_rw().set_prop("_dovi_dynamic_max_content_light_level", static_cast<int64_t>(proc->getDynamicMaxContentLightLevel()));
    dst.frame_props_rw().set_prop("_dovi_static_max_pq", static_cast<int64_t>(proc->getStaticMaxPq()));
    dst.frame_props_rw().set_prop("_dovi_static_max_content_light_level", static_cast<int64_t>(proc->getStaticMaxContentLightLevel()));
    dst.frame_props_rw().set_prop("_dovi_static_max_avg_content_light_level", static_cast<int64_t>(proc->getStaticMaxAvgContentLightLevel()));
    dst.frame_props_rw().set_prop("_dovi_static_master_display_max_luminance", static_cast<int64_t>(proc->getStaticMasterDisplayMaxLuminance()));
    dst.frame_props_rw().set_prop("_dovi_static_master_display_min_luminance", static_cast<int64_t>(proc->getStaticMasterDisplayMinLuminance()));

    // Process using quick and dirty mode (for now, implement full quality mode later)
    if (m_qnd) {
        if (m_blChromaSubSampled && m_elChromaSubSampled) {
            if (m_quarterResolutionEl)
                doAllQuickAndDirty<true, true, true>(dst, blSrc, elSrc, *proc);
            else
                doAllQuickAndDirty<true, true, false>(dst, blSrc, elSrc, *proc);
        } else if (m_blChromaSubSampled && !m_elChromaSubSampled) {
            if (m_quarterResolutionEl)
                doAllQuickAndDirty<true, false, true>(dst, blSrc, elSrc, *proc);
            else
                doAllQuickAndDirty<true, false, false>(dst, blSrc, elSrc, *proc);
        } else if (!m_blChromaSubSampled && m_elChromaSubSampled) {
            if (m_quarterResolutionEl)
                doAllQuickAndDirty<false, true, true>(dst, blSrc, elSrc, *proc);
            else
                doAllQuickAndDirty<false, true, false>(dst, blSrc, elSrc, *proc);
        } else {
            if (m_quarterResolutionEl)
                doAllQuickAndDirty<false, false, true>(dst, blSrc, elSrc, *proc);
            else
                doAllQuickAndDirty<false, false, false>(dst, blSrc, elSrc, *proc);
        }
    } else {
        // Full quality mode with proper upsampling
        ConstFrame blSrc444;
        ConstFrame elSrc444;
        ConstFrame elSrcR = elSrc;
        bool frameChromaSubSampled = m_blChromaSubSampled;

        if (proc->elProcessingEnabled()) {
            if (m_quarterResolutionEl) {
                // Upscale EL to BL resolution
                Frame elUpscaled = upscaleEl(elSrc, m_blVi, core);
                elSrcR = elUpscaled;
            }
            // Handle chroma format mismatches (only for RGB output)
            if (!m_outYUV) {
                if (!m_blChromaSubSampled && m_elChromaSubSampled) {
                    Frame elUp = upsampleChroma(elSrcR, m_blVi, core);
                    elSrc444 = elUp;
                    frameChromaSubSampled = false;
                }
                if (m_blChromaSubSampled && !m_elChromaSubSampled) {
                    Frame blUp = upsampleChroma(blSrc, m_blVi, core);
                    blSrc444 = blUp;
                    frameChromaSubSampled = false;
                }
            }
        } else {
            elSrcR = blSrc;
        }

        if (m_outYUV) {
            // YUV output - write directly to dst, keep original chroma subsampling
            if (m_blChromaSubSampled) {
                applyDovi<true>(dst,
                    blSrc, blSrc,
                    elSrcR, elSrcR, *proc);
            } else {
                applyDovi<false>(dst,
                    blSrc, blSrc,
                    elSrcR, elSrcR, *proc);
            }
        } else {
            // RGB output - create intermediate YUV frame
            VSVideoFormat mezFormat;
            if (m_blChromaSubSampled && !m_elChromaSubSampled) {
                mezFormat = core.query_video_format(cfYUV, stInteger, 16, 0, 0);
            } else {
                mezFormat = core.query_video_format(cfYUV, stInteger, 16,
                    m_blChromaSubSampled ? 1 : 0, m_blChromaSubSampled ? 1 : 0);
            }
            Frame mez = core.new_video_frame(mezFormat, m_blVi.width, m_blVi.height, blSrc);

            // Apply DoVi processing
            if (frameChromaSubSampled) {
                applyDovi<true>(mez,
                    blSrc, blSrc444 ? blSrc444 : blSrc,
                    elSrcR, elSrc444 ? elSrc444 : elSrcR, *proc);
            } else {
                applyDovi<false>(mez,
                    blSrc, blSrc444 ? blSrc444 : blSrc,
                    elSrcR, elSrc444 ? elSrc444 : elSrcR, *proc);
            }

            // Upsample chroma if still subsampled
            ConstFrame mez444 = mez;
            if (frameChromaSubSampled) {
                Frame mezUp = upsampleChroma(mez, m_blVi, core);
                mez444 = mezUp;
            }

            // Convert to RGB
            convert2rgb(dst, mez444, mez444, *proc);
        }
    }

    // Trim processing is only applicable to RGB output
    if (!m_outYUV && proc->trimProcessingEnabled()) {
        applyTrim(dst, dst, *proc);
    }

    return dst;
}

template<bool blChromaSubsampling, bool elChromaSubsampling, bool quarterResolutionEl>
void DoViBakerVS::doAllQuickAndDirty(Frame& dst, const ConstFrame& blSrc, const ConstFrame& elSrc, DoViProcessor& proc) const
{
    const ptrdiff_t blSrcPitchY = blSrc.stride(0) / sizeof(uint16_t);
    const ptrdiff_t elSrcPitchY = elSrc.stride(0) / sizeof(uint16_t);
    const ptrdiff_t dstPitch = dst.stride(0) / sizeof(uint16_t);

    const ptrdiff_t blSrcPitchUV = blSrc.stride(1) / sizeof(uint16_t);
    const int elSrcHeightUV = elSrc.height(1);
    const int elSrcWidthUV = m_hasEl ? m_elVi.width >> (elChromaSubsampling ? 1 : 0) : m_blVi.width >> (blChromaSubsampling ? 1 : 0);
    const ptrdiff_t elSrcPitchUV = elSrc.stride(1) / sizeof(uint16_t);

    constexpr int blYvsElUVshifts = (elChromaSubsampling ? 1 : 0) + (quarterResolutionEl ? 1 : 0);
    std::array<const uint16_t*, (1 << blYvsElUVshifts)> blSrcYp;
    std::array<const uint16_t*, (1 << (elChromaSubsampling ? 1 : 0))> elSrcYp;
    std::array<uint16_t*, (1 << blYvsElUVshifts)> dstRp;
    blSrcYp[0] = reinterpret_cast<const uint16_t*>(blSrc.read_ptr(0));
    elSrcYp[0] = reinterpret_cast<const uint16_t*>(elSrc.read_ptr(0));
    dstRp[0] = reinterpret_cast<uint16_t*>(dst.write_ptr(0));

    constexpr int blUVvsElUVshifts = std::max((quarterResolutionEl ? 1 : 0) + (elChromaSubsampling ? 1 : 0) - (blChromaSubsampling ? 1 : 0), 0);
    std::array<const uint16_t*, (1 << blUVvsElUVshifts)> blSrcUp;
    std::array<const uint16_t*, 1> elSrcUp;
    std::array<uint16_t*, (1 << blYvsElUVshifts)> dstGp;
    blSrcUp[0] = reinterpret_cast<const uint16_t*>(blSrc.read_ptr(1));
    elSrcUp[0] = reinterpret_cast<const uint16_t*>(elSrc.read_ptr(1));
    dstGp[0] = reinterpret_cast<uint16_t*>(dst.write_ptr(1));

    std::array<const uint16_t*, (1 << blUVvsElUVshifts)> blSrcVp;
    std::array<const uint16_t*, 1> elSrcVp;
    std::array<uint16_t*, (1 << blYvsElUVshifts)> dstBp;
    blSrcVp[0] = reinterpret_cast<const uint16_t*>(blSrc.read_ptr(2));
    elSrcVp[0] = reinterpret_cast<const uint16_t*>(elSrc.read_ptr(2));
    dstBp[0] = reinterpret_cast<uint16_t*>(dst.write_ptr(2));

    // Initialize pointer arrays
    for (size_t i = 1; i < elSrcVp.size(); i++) {
        elSrcUp[i] = elSrcUp[i - 1] + elSrcPitchUV;
        elSrcVp[i] = elSrcVp[i - 1] + elSrcPitchUV;
    }
    for (size_t i = 1; i < elSrcYp.size(); i++) {
        elSrcYp[i] = elSrcYp[i - 1] + elSrcPitchY;
    }
    for (size_t i = 1; i < blSrcVp.size(); i++) {
        blSrcUp[i] = blSrcUp[i - 1] + blSrcPitchUV;
        blSrcVp[i] = blSrcVp[i - 1] + blSrcPitchUV;
    }
    for (size_t i = 1; i < dstBp.size(); i++) {
        blSrcYp[i] = blSrcYp[i - 1] + blSrcPitchY;
        dstRp[i] = dstRp[i - 1] + dstPitch;
        dstGp[i] = dstGp[i - 1] + dstPitch;
        dstBp[i] = dstBp[i - 1] + dstPitch;
    }

    for (int heluv = 0; heluv < elSrcHeightUV; heluv++) {
        for (int weluv = 0; weluv < elSrcWidthUV; weluv++) {

            const uint16_t elu = elSrcUp[0][weluv];
            const uint16_t elv = elSrcVp[0][weluv];

            for (int hDbluv = 0; hDbluv < (1 << blUVvsElUVshifts); hDbluv++) {
                for (int wDbluv = 0; wDbluv < (1 << blUVvsElUVshifts); wDbluv++) {

                    int wbluv = (weluv << blUVvsElUVshifts) + wDbluv;
                    const uint16_t blu = blSrcUp[hDbluv][wbluv];
                    const uint16_t blv = blSrcVp[hDbluv][wbluv];

                    int hDbluvy = hDbluv << (blChromaSubsampling ? 1 : 0);
                    int wbluvy = wbluv << (blChromaSubsampling ? 1 : 0);
                    const uint16_t mmrbly = blSrcYp[hDbluvy][wbluvy];

                    const uint16_t u = proc.processSampleU(blu, elu, mmrbly, blu, blv);
                    const uint16_t v = proc.processSampleV(blv, elv, mmrbly, blu, blv);

                    for (int hDbly = 0; hDbly < (blChromaSubsampling ? 2 : 1); hDbly++) {
                        for (int wDbly = 0; wDbly < (blChromaSubsampling ? 2 : 1); wDbly++) {

                            int hDDbly = hDbluvy + hDbly;
                            int wbly = wbluvy + wDbly;
                            const uint16_t bly = blSrcYp[hDDbly][wbly];

                            int hDely = hDDbly >> (quarterResolutionEl ? 1 : 0);
                            int wely = wbly >> (quarterResolutionEl ? 1 : 0);
                            const uint16_t ely = elSrcYp[hDely][wely];

                            const uint16_t y = proc.processSampleY(bly, ely);
                            proc.sample2rgb(dstRp[hDDbly][wbly], dstGp[hDDbly][wbly], dstBp[hDDbly][wbly], y, u, v);
                        }
                    }
                }
            }
        }

        // Advance pointers
        for (size_t i = 0; i < elSrcVp.size(); i++) {
            elSrcVp[i] += elSrcPitchUV;
            elSrcUp[i] += elSrcPitchUV;
        }
        for (size_t i = 0; i < elSrcYp.size(); i++) {
            elSrcYp[i] += elSrcPitchY * elSrcYp.size();
        }
        for (size_t i = 0; i < blSrcVp.size(); i++) {
            blSrcVp[i] += blSrcPitchUV * blSrcVp.size();
            blSrcUp[i] += blSrcPitchUV * blSrcVp.size();
        }
        for (size_t i = 0; i < blSrcYp.size(); i++) {
            blSrcYp[i] += blSrcPitchY * blSrcYp.size();
            dstRp[i] += dstPitch * blSrcYp.size();
            dstGp[i] += dstPitch * blSrcYp.size();
            dstBp[i] += dstPitch * blSrcYp.size();
        }
    }
}

// Vertical upsampling - produces 2 output rows per input row
template<int vertLen, int nD>
void DoViBakerVS::upsampleVert(Frame& dst, const ConstFrame& src, int plane,
    const std::array<int, vertLen>& Dn0p,
    const upscaler_t evenUpscaler, const upscaler_t oddUpscaler)
{
    const int srcHeight = src.height(plane);
    const int srcWidth = src.width(plane);
    const ptrdiff_t srcPitch = src.stride(plane) / sizeof(uint16_t);
    const uint16_t* srcPb = reinterpret_cast<const uint16_t*>(src.read_ptr(plane));

    const ptrdiff_t dstPitch = dst.stride(plane) / sizeof(uint16_t);
    uint16_t* dstPeven = reinterpret_cast<uint16_t*>(dst.write_ptr(plane));
    uint16_t* dstPodd = dstPeven + dstPitch;

    std::array<const uint16_t*, vertLen> srcP;
    std::array<uint16_t, vertLen> value;
    auto& srcP0 = srcP[nD];

    for (int h0 = 0; h0 < srcHeight; h0++) {
        // Border handling: clamp to [0, srcHeight-1]
        for (int i = 0; i < nD; i++) {
            int factor = std::max(h0 + Dn0p[i], 0);
            srcP[i] = srcPb + factor * srcPitch;
        }
        srcP0 = srcPb + h0 * srcPitch;
        for (int i = nD + 1; i < vertLen; i++) {
            int factor = std::min(h0 + Dn0p[i], srcHeight - 1);
            srcP[i] = srcPb + factor * srcPitch;
        }

        for (int w = 0; w < srcWidth; w++) {
            for (int i = 0; i < vertLen; i++) {
                value[i] = srcP[i][w];
            }
            dstPeven[w] = evenUpscaler(&value[0], nD);
            dstPodd[w] = oddUpscaler(&value[0], nD);
        }

        dstPeven += 2 * dstPitch;
        dstPodd += 2 * dstPitch;
    }
}

// Horizontal upsampling - produces 2 output columns per input column
template<int vertLen, int nD>
void DoViBakerVS::upsampleHorz(Frame& dst, const ConstFrame& src, int plane,
    const std::array<int, vertLen>& Dn0p,
    const upscaler_t evenUpscaler, const upscaler_t oddUpscaler)
{
    const int srcHeight = src.height(plane);
    const int srcWidth = src.width(plane);
    const ptrdiff_t srcPitch = src.stride(plane) / sizeof(uint16_t);
    const uint16_t* srcP = reinterpret_cast<const uint16_t*>(src.read_ptr(plane));

    const ptrdiff_t dstPitch = dst.stride(plane) / sizeof(uint16_t);
    uint16_t* dstP = reinterpret_cast<uint16_t*>(dst.write_ptr(plane));

    static const int pD = vertLen - nD - 1;
    std::array<uint16_t, vertLen> value;

    for (int h = 0; h < srcHeight; h++) {
        // Center region - no border handling needed
        for (int w = nD; w < srcWidth - pD; w++) {
            dstP[2 * w] = evenUpscaler(&srcP[w - nD], nD);
            dstP[2 * w + 1] = oddUpscaler(&srcP[w - nD], nD);
        }

        // Left edge - clamp left samples
        for (int w = 0; w < nD; w++) {
            for (int i = 0; i < nD; i++) {
                int wd = std::max(w + Dn0p[i], 0);
                value[i] = srcP[wd];
            }
            std::copy_n(&srcP[w], pD + 1, &value[nD]);
            dstP[2 * w] = evenUpscaler(&value[0], nD);
            dstP[2 * w + 1] = oddUpscaler(&value[0], nD);
        }

        // Right edge - clamp right samples
        for (int w = srcWidth - pD; w < srcWidth; w++) {
            for (int i = nD + 1; i < vertLen; i++) {
                int wd = std::min(w + Dn0p[i], srcWidth - 1);
                value[i] = srcP[wd];
            }
            std::copy_n(&srcP[w - nD], nD + 1, &value[0]);
            dstP[2 * w] = evenUpscaler(&value[0], nD);
            dstP[2 * w + 1] = oddUpscaler(&value[0], nD);
        }

        srcP += srcPitch;
        dstP += dstPitch;
    }
}

// Upscale quarter-resolution EL to full resolution (2x in each dimension)
Frame DoViBakerVS::upscaleEl(const ConstFrame& src, const VSVideoInfo& dstVi, const Core& core)
{
    // Create intermediate frame at half target width (vertical upsampling first)
    VSVideoFormat mezFormat = core.query_video_format(cfYUV, stInteger, 16,
        m_elChromaSubSampled ? 1 : 0, m_elChromaSubSampled ? 1 : 0);
    Frame mez = core.new_video_frame(mezFormat, dstVi.width / 2, dstVi.height, src);

    // Step 1: Vertical upsampling (5-tap for luma, 4-tap for chroma)
    upsampleVert<5, 2>(mez, src, 0, {-2, -1, 0, 1, 2},
        &DoViProcessor::upsampleLumaEven, &DoViProcessor::upsampleLumaOdd);
    upsampleVert<4, 1>(mez, src, 1, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);
    upsampleVert<4, 1>(mez, src, 2, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);

    // Create output frame at full target size
    Frame dst = core.new_video_frame(mezFormat, dstVi.width, dstVi.height, src);

    // Step 2: Horizontal upsampling
    upsampleHorz<5, 2>(dst, mez, 0, {-2, -1, 0, 1, 2},
        &DoViProcessor::upsampleLumaEven, &DoViProcessor::upsampleLumaOdd);
    upsampleHorz<4, 1>(dst, mez, 1, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);
    upsampleHorz<4, 1>(dst, mez, 2, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);

    return dst;
}

// Upsample chroma from 4:2:0 to 4:4:4
Frame DoViBakerVS::upsampleChroma(const ConstFrame& src, const VSVideoInfo& dstVi, const Core& core)
{
    // Create intermediate frame for vertical upsampling (half width, full height)
    VSVideoFormat mezFormat = core.query_video_format(cfYUV, stInteger, 16, 1, 0);
    Frame mez = core.new_video_frame(mezFormat, dstVi.width, dstVi.height, src);

    // Copy luma directly
    const uint16_t* srcY = reinterpret_cast<const uint16_t*>(src.read_ptr(0));
    uint16_t* dstY = reinterpret_cast<uint16_t*>(mez.write_ptr(0));
    const ptrdiff_t srcPitchY = src.stride(0) / sizeof(uint16_t);
    const ptrdiff_t dstPitchY = mez.stride(0) / sizeof(uint16_t);
    for (int h = 0; h < dstVi.height; h++) {
        std::memcpy(dstY, srcY, dstVi.width * sizeof(uint16_t));
        srcY += srcPitchY;
        dstY += dstPitchY;
    }

    // Vertical upsampling for U and V
    upsampleVert<4, 1>(mez, src, 1, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);
    upsampleVert<4, 1>(mez, src, 2, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);

    // Create output frame at 4:4:4
    VSVideoFormat dstFormat = core.query_video_format(cfYUV, stInteger, 16, 0, 0);
    Frame dst = core.new_video_frame(dstFormat, dstVi.width, dstVi.height, src);

    // Copy luma directly to output
    srcY = reinterpret_cast<const uint16_t*>(mez.read_ptr(0));
    dstY = reinterpret_cast<uint16_t*>(dst.write_ptr(0));
    const ptrdiff_t mezPitchY = mez.stride(0) / sizeof(uint16_t);
    const ptrdiff_t outPitchY = dst.stride(0) / sizeof(uint16_t);
    for (int h = 0; h < dstVi.height; h++) {
        std::memcpy(dstY, srcY, dstVi.width * sizeof(uint16_t));
        srcY += mezPitchY;
        dstY += outPitchY;
    }

    // Horizontal upsampling for U and V
    upsampleHorz<4, 1>(dst, mez, 1, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);
    upsampleHorz<4, 1>(dst, mez, 2, {-1, 0, 1, 2},
        &DoViProcessor::upsampleChromaEven, &DoViProcessor::upsampleChromaOdd);

    return dst;
}

// Apply DoVi processing with proper chroma handling
template<bool chromaSubsampling>
void DoViBakerVS::applyDovi(Frame& dst, const ConstFrame& blSrcY, const ConstFrame& blSrcUV,
                             const ConstFrame& elSrcY, const ConstFrame& elSrcUV, DoViProcessor& proc) const
{
    const ptrdiff_t blSrcPitchY = blSrcY.stride(0) / sizeof(uint16_t);
    const ptrdiff_t elSrcPitchY = elSrcY.stride(0) / sizeof(uint16_t);
    const ptrdiff_t dstPitchY = dst.stride(0) / sizeof(uint16_t);

    std::array<const uint16_t*, chromaSubsampling ? 2 : 1> blSrcYp;
    std::array<const uint16_t*, chromaSubsampling ? 2 : 1> elSrcYp;
    std::array<uint16_t*, chromaSubsampling ? 2 : 1> dstYp;

    blSrcYp[0] = reinterpret_cast<const uint16_t*>(blSrcY.read_ptr(0));
    elSrcYp[0] = reinterpret_cast<const uint16_t*>(elSrcY.read_ptr(0));
    dstYp[0] = reinterpret_cast<uint16_t*>(dst.write_ptr(0));
    if constexpr (chromaSubsampling) {
        blSrcYp[1] = blSrcYp[0] + blSrcPitchY;
        elSrcYp[1] = elSrcYp[0] + elSrcPitchY;
        dstYp[1] = dstYp[0] + dstPitchY;
    }

    const int blSrcHeightUV = blSrcUV.height(1);
    const int blSrcWidthUV = blSrcUV.width(1);
    const ptrdiff_t blSrcPitchUV = blSrcUV.stride(1) / sizeof(uint16_t);
    const ptrdiff_t elSrcPitchUV = elSrcUV.stride(1) / sizeof(uint16_t);
    const ptrdiff_t dstPitchUV = dst.stride(1) / sizeof(uint16_t);

    const uint16_t* blSrcUp = reinterpret_cast<const uint16_t*>(blSrcUV.read_ptr(1));
    const uint16_t* elSrcUp = reinterpret_cast<const uint16_t*>(elSrcUV.read_ptr(1));
    uint16_t* dstUp = reinterpret_cast<uint16_t*>(dst.write_ptr(1));

    const uint16_t* blSrcVp = reinterpret_cast<const uint16_t*>(blSrcUV.read_ptr(2));
    const uint16_t* elSrcVp = reinterpret_cast<const uint16_t*>(elSrcUV.read_ptr(2));
    uint16_t* dstVp = reinterpret_cast<uint16_t*>(dst.write_ptr(2));

    constexpr int csVal = chromaSubsampling ? 1 : 0;

    for (int huv = 0; huv < blSrcHeightUV; huv++) {
        if constexpr (chromaSubsampling) {
            // Left edge
            int wuv = 0;
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    const int w = 2 * wuv + i;
                    dstYp[j][w] = proc.processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
                }
            }
            int mmrBlY1 = 3 * blSrcYp[0][2 * wuv] + blSrcYp[0][2 * wuv + 1] + 2;
            int mmrBlY2 = 3 * blSrcYp[1][2 * wuv] + blSrcYp[1][2 * wuv + 1] + 2;
            uint16_t mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;

            dstUp[wuv] = proc.processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
            dstVp[wuv] = proc.processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
        }

        // Center region
        for (int wuv = csVal; wuv < blSrcWidthUV - csVal; wuv++) {
            for (int j = 0; j < (chromaSubsampling ? 2 : 1); j++) {
                for (int i = 0; i < (chromaSubsampling ? 2 : 1); i++) {
                    const int w = (chromaSubsampling ? 2 : 1) * wuv + i;
                    dstYp[j][w] = proc.processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
                }
            }
            uint16_t mmrBlY;
            if constexpr (chromaSubsampling) {
                int mmrBlY1 = blSrcYp[0][2 * wuv - 1] + 2 * blSrcYp[0][2 * wuv] + blSrcYp[0][2 * wuv + 1] + 2;
                int mmrBlY2 = blSrcYp[1][2 * wuv - 1] + 2 * blSrcYp[1][2 * wuv] + blSrcYp[1][2 * wuv + 1] + 2;
                mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;
            } else {
                mmrBlY = blSrcYp[0][wuv];
            }
            dstUp[wuv] = proc.processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
            dstVp[wuv] = proc.processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
        }

        if constexpr (chromaSubsampling) {
            // Right edge
            int wuv = blSrcWidthUV - 1;
            for (int j = 0; j < 2; j++) {
                for (int i = 0; i < 2; i++) {
                    const int w = 2 * wuv + i;
                    dstYp[j][w] = proc.processSampleY(blSrcYp[j][w], elSrcYp[j][w]);
                }
            }
            int mmrBlY1 = blSrcYp[0][2 * wuv - 1] + 3 * blSrcYp[0][2 * wuv] + 2;
            int mmrBlY2 = blSrcYp[1][2 * wuv - 1] + 3 * blSrcYp[1][2 * wuv] + 2;
            uint16_t mmrBlY = ((mmrBlY1 >> 2) + (mmrBlY2 >> 2) + 1) >> 1;

            dstUp[wuv] = proc.processSampleU(blSrcUp[wuv], elSrcUp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
            dstVp[wuv] = proc.processSampleV(blSrcVp[wuv], elSrcVp[wuv], mmrBlY, blSrcUp[wuv], blSrcVp[wuv]);
        }

        // Advance row pointers
        for (int i = 0; i < (chromaSubsampling ? 2 : 1); i++) {
            blSrcYp[i] += blSrcPitchY * (chromaSubsampling ? 2 : 1);
            elSrcYp[i] += elSrcPitchY * (chromaSubsampling ? 2 : 1);
            dstYp[i] += dstPitchY * (chromaSubsampling ? 2 : 1);
        }
        blSrcUp += blSrcPitchUV;
        blSrcVp += blSrcPitchUV;
        elSrcUp += elSrcPitchUV;
        elSrcVp += elSrcPitchUV;
        dstUp += dstPitchUV;
        dstVp += dstPitchUV;
    }
}

// Convert processed YUV to RGB
void DoViBakerVS::convert2rgb(Frame& dst, const ConstFrame& srcY, const ConstFrame& srcUV, DoViProcessor& proc) const
{
    const ptrdiff_t srcPitchY = srcY.stride(0) / sizeof(uint16_t);
    const ptrdiff_t dstPitch = dst.stride(0) / sizeof(uint16_t);

    const uint16_t* srcYp = reinterpret_cast<const uint16_t*>(srcY.read_ptr(0));
    uint16_t* dstRp = reinterpret_cast<uint16_t*>(dst.write_ptr(0));

    const int srcHeightUV = srcUV.height(1);
    const int srcWidthUV = srcUV.width(1);
    const ptrdiff_t srcPitchUV = srcUV.stride(1) / sizeof(uint16_t);

    const uint16_t* srcUp = reinterpret_cast<const uint16_t*>(srcUV.read_ptr(1));
    uint16_t* dstGp = reinterpret_cast<uint16_t*>(dst.write_ptr(1));

    const uint16_t* srcVp = reinterpret_cast<const uint16_t*>(srcUV.read_ptr(2));
    uint16_t* dstBp = reinterpret_cast<uint16_t*>(dst.write_ptr(2));

    for (int huv = 0; huv < srcHeightUV; huv++) {
        for (int wuv = 0; wuv < srcWidthUV; wuv++) {
            proc.sample2rgb(dstRp[wuv], dstGp[wuv], dstBp[wuv], srcYp[wuv], srcUp[wuv], srcVp[wuv]);
        }

        srcYp += srcPitchY;
        srcUp += srcPitchUV;
        srcVp += srcPitchUV;

        dstRp += dstPitch;
        dstGp += dstPitch;
        dstBp += dstPitch;
    }
}

void DoViBakerVS::applyTrim(Frame& dst, const ConstFrame& src, DoViProcessor& proc) const
{
    const int width = m_vi.width;
    const int height = m_vi.height;

    const uint16_t* srcP[3];
    ptrdiff_t srcPitch[3];
    uint16_t* dstP[3];
    ptrdiff_t dstPitch[3];

    for (int p = 0; p < 3; ++p) {
        srcP[p] = reinterpret_cast<const uint16_t*>(src.read_ptr(p));
        srcPitch[p] = src.stride(p) / sizeof(uint16_t);
        dstP[p] = reinterpret_cast<uint16_t*>(dst.write_ptr(p));
        dstPitch[p] = dst.stride(p) / sizeof(uint16_t);
    }

    for (int h = 0; h < height; ++h) {
        for (int w = 0; w < width; ++w) {
            proc.processTrim(dstP[0][w], dstP[1][w], dstP[2][w], srcP[0][w], srcP[1][w], srcP[2][w]);
        }

        for (int p = 0; p < 3; ++p) {
            srcP[p] += srcPitch[p];
            dstP[p] += dstPitch[p];
        }
    }
}

// Explicit template instantiations
template void DoViBakerVS::doAllQuickAndDirty<true, true, true>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<true, true, false>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<true, false, true>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<true, false, false>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<false, true, true>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<false, true, false>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<false, false, true>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::doAllQuickAndDirty<false, false, false>(Frame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;

// Upsampling template instantiations
template void DoViBakerVS::upsampleVert<5, 2>(Frame&, const ConstFrame&, int, const std::array<int, 5>&, const upscaler_t, const upscaler_t);
template void DoViBakerVS::upsampleVert<4, 1>(Frame&, const ConstFrame&, int, const std::array<int, 4>&, const upscaler_t, const upscaler_t);
template void DoViBakerVS::upsampleHorz<5, 2>(Frame&, const ConstFrame&, int, const std::array<int, 5>&, const upscaler_t, const upscaler_t);
template void DoViBakerVS::upsampleHorz<4, 1>(Frame&, const ConstFrame&, int, const std::array<int, 4>&, const upscaler_t, const upscaler_t);

// DoVi processing template instantiations
template void DoViBakerVS::applyDovi<true>(Frame&, const ConstFrame&, const ConstFrame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
template void DoViBakerVS::applyDovi<false>(Frame&, const ConstFrame&, const ConstFrame&, const ConstFrame&, const ConstFrame&, DoViProcessor&) const;
