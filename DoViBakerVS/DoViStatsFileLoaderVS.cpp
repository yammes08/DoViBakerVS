#include "DoViStatsFileLoaderVS.h"
#include "DoViProcessor.h"
#include "VSHelper4.h"
#include <fstream>
#include <sstream>
#include <deque>
#include <algorithm>
#include <stdexcept>

void DoViStatsFileLoaderVS::init(const ConstMap& in, const Map& out, const Core& core)
{
    m_clip = in.get_prop<FilterNode>("clip");
    m_vi = m_clip.video_info();

    if (!vsh::isConstantVideoFormat(&m_vi))
        throw std::runtime_error("DoViStatsFileLoader: input must have constant format");

    // Get file paths
    const char* maxPqFile = in.get_prop<const char*>("statsFile");
    const char* sceneCutFile = nullptr;
    if (in.contains("sceneCutsFile")) {
        sceneCutFile = in.get_prop<const char*>("sceneCutsFile");
    }

    // Parse stats file
    uint32_t frame = 0, isLastFrameInScene, frameMaxPq, frameMinPq, firstFrameNextScene = 0;
    uint16_t sceneMaxPq = 0, sceneMinPq = 0xFFFF;
    float scale;
    std::deque<float> sceneScales;
    std::ifstream fpStats, fpSceneCut;
    std::string line, segment;

    fpStats.open(maxPqFile, std::ifstream::in);
    if (!fpStats.is_open()) {
        throw std::runtime_error(std::string("DoViStatsFileLoader: cannot find stats file ") + maxPqFile);
    }

    if (sceneCutFile && strlen(sceneCutFile) > 0) {
        fpSceneCut.open(sceneCutFile, std::ifstream::in);
        if (!fpSceneCut.is_open()) {
            throw std::runtime_error(std::string("DoViStatsFileLoader: cannot find scene cut file ") + sceneCutFile);
        }
        while (firstFrameNextScene == 0) {
            if (!(fpSceneCut >> firstFrameNextScene)) {
                throw std::runtime_error(std::string("DoViStatsFileLoader: error reading scene cut file ") + sceneCutFile);
            }
        }
    }

    while (std::getline(fpStats, line)) {
        std::istringstream ssline(line);
        if (std::getline(ssline, segment, ' ')) {
            frame = std::atoi(segment.c_str());
        } else {
            throw std::runtime_error("DoViStatsFileLoader: error reading frame number from stats file");
        }
        if (std::getline(ssline, segment, ' ')) {
            isLastFrameInScene = std::atoi(segment.c_str());
        } else {
            throw std::runtime_error("DoViStatsFileLoader: error reading scene change from stats file");
        }
        if (std::getline(ssline, segment, ' ')) {
            frameMaxPq = std::atoi(segment.c_str());
        } else {
            throw std::runtime_error("DoViStatsFileLoader: error reading maxPq from stats file");
        }
        if (std::getline(ssline, segment, ' ')) {
            frameMinPq = std::atoi(segment.c_str());
        } else {
            throw std::runtime_error("DoViStatsFileLoader: error reading minPq from stats file");
        }
        if (std::getline(ssline, segment, ' ')) {
            scale = static_cast<float>(std::atof(segment.c_str()));
            sceneScales.push_back(scale);
        }

        if (frameMaxPq > sceneMaxPq) sceneMaxPq = static_cast<uint16_t>(frameMaxPq);
        if (sceneMaxPq > m_staticMaxPq) m_staticMaxPq = sceneMaxPq;
        if (frameMinPq < sceneMinPq) sceneMinPq = static_cast<uint16_t>(frameMinPq);

        if (fpSceneCut.is_open()) {
            if (firstFrameNextScene != frame + 1) continue;
        } else if (!isLastFrameInScene) continue;

        float sceneScaleMedian = 1.0f;
        if (!sceneScales.empty()) {
            std::sort(sceneScales.begin(), sceneScales.end());
            sceneScaleMedian = sceneScales.at(sceneScales.size() / 2);
            sceneScales.clear();
        }

        m_sceneMaxSignal.push_back(std::tuple(frame + 1, sceneMaxPq, sceneMinPq, sceneScaleMedian));
        sceneMaxPq = 0;
        sceneMinPq = 0xFFFF;

        if (fpSceneCut.is_open()) {
            if (!(fpSceneCut >> firstFrameNextScene)) {
                firstFrameNextScene = static_cast<uint32_t>(m_vi.numFrames);
            }
        }
    }

    float sceneScaleMedian = 1.0f;
    if (!sceneScales.empty()) {
        std::sort(sceneScales.begin(), sceneScales.end());
        sceneScaleMedian = sceneScales.at(sceneScales.size() / 2);
    }

    m_sceneMaxSignal.push_back(std::tuple(frame + 1, sceneMaxPq, sceneMinPq, sceneScaleMedian));
    m_staticMaxCll = static_cast<uint16_t>(DoViProcessor::pq2nits(m_staticMaxPq) + 0.5f);

    if (m_vi.numFrames != static_cast<int>(frame + 1)) {
        throw std::runtime_error(std::string("DoViStatsFileLoader: clip length does not match stats file ") + maxPqFile);
    }

    fpStats.close();
    if (fpSceneCut.is_open()) fpSceneCut.close();

    // Register filter
    create_video_filter(out, m_vi, fmParallel, simple_dep(m_clip, rpStrictSpatial), core);
}

ConstFrame DoViStatsFileLoaderVS::get_frame_initial(int n, const Core& core, const FrameContext& frame_context, void*)
{
    frame_context.request_frame(n, m_clip);
    return nullptr;
}

ConstFrame DoViStatsFileLoaderVS::get_frame(int n, const Core& core, const FrameContext& frame_context, void*)
{
    ConstFrame src = frame_context.get_frame(n, m_clip);

    // Make a copy so we can modify frame properties
    Frame dst = core.copy_frame(src);

    // Reset scene tracking on backwards navigation
    if (m_previousFrame > static_cast<uint32_t>(n))
        m_currentScene = 0;
    m_previousFrame = static_cast<uint32_t>(n);

    // Find current scene
    while (std::get<0>(m_sceneMaxSignal.at(m_currentScene)) <= static_cast<uint32_t>(n)) {
        m_currentScene++;
    }

    uint16_t maxPq = std::get<1>(m_sceneMaxSignal.at(m_currentScene));
    uint16_t maxCll = static_cast<uint16_t>(DoViProcessor::pq2nits(maxPq) + 0.5f);
    uint16_t minPq = std::get<2>(m_sceneMaxSignal.at(m_currentScene));
    float scale = std::get<3>(m_sceneMaxSignal.at(m_currentScene));

    // Set frame properties
    dst.frame_props_rw().set_prop("_dovi_dynamic_min_pq", static_cast<int64_t>(minPq));
    dst.frame_props_rw().set_prop("_dovi_dynamic_max_pq", static_cast<int64_t>(maxPq));
    dst.frame_props_rw().set_prop("_dovi_dynamic_max_content_light_level", static_cast<int64_t>(maxCll));
    dst.frame_props_rw().set_prop("_dovi_static_max_pq", static_cast<int64_t>(m_staticMaxPq));
    dst.frame_props_rw().set_prop("_dovi_static_max_content_light_level", static_cast<int64_t>(m_staticMaxCll));
    dst.frame_props_rw().set_prop("_dovi_dynamic_luminosity_scale", static_cast<double>(scale));

    dst.frame_props_rw().set_prop("_SceneChangeNext",
        static_cast<int64_t>(std::get<0>(m_sceneMaxSignal.at(m_currentScene)) == static_cast<uint32_t>(n + 1) ? 1 : 0));

    if (m_currentScene > 0) {
        dst.frame_props_rw().set_prop("_SceneChangePrev",
            static_cast<int64_t>(std::get<0>(m_sceneMaxSignal.at(m_currentScene - 1)) == static_cast<uint32_t>(n) ? 1 : 0));
    } else {
        dst.frame_props_rw().set_prop("_SceneChangePrev", static_cast<int64_t>(0));
    }

    return dst;
}
