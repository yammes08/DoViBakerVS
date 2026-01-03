#include "DoViCubesVS.h"
#include "VSHelper4.h"
#include <stdexcept>
#include <climits>
#include <filesystem>
#include <string>

void DoViCubesVS::init(const ConstMap& in, const Map& out, const Core& core)
{
    m_clip = in.get_prop<FilterNode>("clip");
    m_vi = m_clip.video_info();

    if (!vsh::isConstantVideoFormat(&m_vi))
        throw std::runtime_error("DoViCubes: input must have constant format");

    if (m_vi.format.colorFamily != cfRGB)
        throw std::runtime_error("DoViCubes: input must be RGB");

    if (m_vi.format.sampleType != stInteger || m_vi.format.bytesPerSample != 2)
        throw std::runtime_error("DoViCubes: input must be 16-bit integer");

    m_fullrange = in.get_prop<int64_t>("fullrange", map::default_val(1LL)) != 0;

    // Get cube file paths and mcll thresholds
    int numCubes = in.num_elements("cubes");
    int numMclls = in.num_elements("mclls");

    if (numCubes <= 0)
        throw std::runtime_error("DoViCubes: at least one cube file must be specified");

    if (numCubes != numMclls)
        throw std::runtime_error("DoViCubes: number of cubes must match number of mclls");

    // Get optional base path
    std::string basePath;
    if (in.contains("cubes_basepath")) {
        basePath = in.get_prop<const char*>("cubes_basepath");
        if (!basePath.empty() && basePath.back() != '/' && basePath.back() != '\\') {
            basePath += '/';
        }
    }

    // Setup timecube parameters
    timecube_filter_params params{};
    params.width = static_cast<unsigned>(m_vi.width);
    params.height = static_cast<unsigned>(m_vi.height);
    params.src_type = TIMECUBE_PIXEL_WORD;
    params.src_depth = static_cast<unsigned>(m_vi.format.bitsPerSample);
    params.src_range = TIMECUBE_RANGE_FULL;
    params.dst_type = TIMECUBE_PIXEL_WORD;
    params.dst_depth = static_cast<unsigned>(m_vi.format.bitsPerSample);
    params.dst_range = m_fullrange ? TIMECUBE_RANGE_FULL : TIMECUBE_RANGE_LIMITED;
    params.interp = TIMECUBE_INTERP_TETRA;
    params.cpu = static_cast<timecube_cpu_type_e>(INT_MAX);

    // Load all LUTs
    for (int i = 0; i < numCubes; ++i) {
        const char* cubeName = in.get_prop<const char*>("cubes", i);
        int64_t mcll = in.get_prop<int64_t>("mclls", i);

        std::string cubePath = basePath + cubeName;

        if (!std::filesystem::exists(std::filesystem::path(cubePath))) {
            throw std::runtime_error(std::string("DoViCubes: cannot find cube file ") + cubePath);
        }

        std::unique_ptr<timecube_lut, TimecubeLutFree> cube{ timecube_lut_from_file(cubePath.c_str()) };
        if (!cube) {
            throw std::runtime_error(std::string("DoViCubes: error reading LUT from file ") + cubePath);
        }

        timecube_filter* filter = timecube_filter_create(cube.get(), &params);
        if (!filter) {
            throw std::runtime_error(std::string("DoViCubes: error creating LUT from file ") + cubePath);
        }

        m_luts.emplace_back(static_cast<uint16_t>(mcll), std::unique_ptr<timecube_filter, TimecubeFilterFree>(filter));
    }

    create_video_filter(out, m_vi, fmParallel, simple_dep(m_clip, rpStrictSpatial), core);
}

ConstFrame DoViCubesVS::get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*)
{
    frame_context.request_frame(n, m_clip);
    return nullptr;
}

ConstFrame DoViCubesVS::get_frame(int n, const Core& core, const FrameContext& frame_context, void*)
{
    ConstFrame src = frame_context.get_frame(n, m_clip);
    Frame dst = core.new_video_frame(m_vi.format, m_vi.width, m_vi.height, src);

    // Get max content light level from frame properties
    uint16_t maxCll = 0;
    if (src.frame_props_ro().contains("_dovi_dynamic_max_content_light_level")) {
        maxCll = static_cast<uint16_t>(src.frame_props_ro().get_prop<int64_t>("_dovi_dynamic_max_content_light_level"));
    } else {
        throw std::runtime_error("DoViCubes: Expected frame property '_dovi_dynamic_max_content_light_level' not available");
    }

    // Select appropriate LUT based on maxCll
    const timecube_filter* selectedLut = m_luts.back().second.get();
    for (size_t i = 1; i < m_luts.size(); ++i) {
        if (maxCll <= m_luts[i].first) {
            selectedLut = m_luts[i - 1].second.get();
            break;
        }
    }

    applyLut(dst, src, selectedLut);

    return dst;
}

void DoViCubesVS::applyLut(Frame& dst, const ConstFrame& src, const timecube_filter* lut) const
{
    const void* src_p[3];
    ptrdiff_t src_stride[3];
    void* dst_p[3];
    ptrdiff_t dst_stride[3];

    for (int p = 0; p < 3; ++p) {
        src_p[p] = src.read_ptr(p);
        src_stride[p] = src.stride(p);
        dst_p[p] = dst.write_ptr(p);
        dst_stride[p] = dst.stride(p);
    }

    std::unique_ptr<void, decltype(&vsh::vsh_aligned_free)> tmp{ nullptr, vsh::vsh_aligned_free };
    tmp.reset(vsh::vsh_aligned_malloc(timecube_filter_get_tmp_size(lut), 64));

    timecube_filter_apply(lut, src_p, src_stride, dst_p, dst_stride, tmp.get());
}
