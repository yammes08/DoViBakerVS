// Minimal stub for VapourSynth-only builds
// This provides just enough to compile DoViProcessor without the full AviSynth SDK

#pragma once

// Stub class - DoViProcessor only uses env->ThrowError() when env is non-null,
// and VS code always passes nullptr for env
class IScriptEnvironment {
public:
    virtual void ThrowError(const char* message) = 0;
};
