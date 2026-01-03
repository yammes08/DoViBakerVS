#include "vsxx4_pluginmain.h"
#include "DoViBakerVS.h"
#include "DoViTonemapVS.h"
#include "DoViCubesVS.h"
#include "DoViStatsFileLoaderVS.h"

const PluginInfo4 g_plugin_info4 = {
    "com.dovibaker.vs",
    "dovi",
    "DoViBaker for VapourSynth",
    1,
    {
        {
            &FilterBase::filter_create<DoViBakerVS>,
            "Baker",
            "bl:vnode;"
            "el:vnode:opt;"
            "rpu:data:opt;"
            "trimPq:int:opt;"
            "targetMaxNits:float:opt;"
            "targetMinNits:float:opt;"
            "qnd:int:opt;"
            "rgbProof:int:opt;"
            "nlqProof:int:opt;"
            "outYUV:int:opt;"
            "sourceProfile:int:opt;",
            "clip:vnode;"
        },
        {
            &FilterBase::filter_create<DoViTonemapVS>,
            "Tonemap",
            "clip:vnode;"
            "targetMaxNits:float:opt;"
            "targetMinNits:float:opt;"
            "masterMaxNits:float:opt;"
            "masterMinNits:float:opt;"
            "lumScale:float:opt;"
            "kneeOffset:float:opt;"
            "normalizeOutput:int:opt;",
            "clip:vnode;"
        },
        {
            &FilterBase::filter_create<DoViCubesVS>,
            "Cubes",
            "clip:vnode;"
            "cubes:data[];"
            "mclls:int[];"
            "cubes_basepath:data:opt;"
            "fullrange:int:opt;",
            "clip:vnode;"
        },
        {
            &FilterBase::filter_create<DoViStatsFileLoaderVS>,
            "StatsFileLoader",
            "clip:vnode;"
            "statsFile:data;"
            "sceneCutsFile:data:opt;",
            "clip:vnode;"
        }
    }
};
