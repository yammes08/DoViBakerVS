# DoViBaker for VapourSynth

This is a VapourSynth port of [DoViBaker](https://github.com/erazortt/DoViBaker), providing tools for processing Dolby Vision HDR content.

> **Note:** This is an early release with limited testing. Please report any issues on the [issue tracker](https://github.com/yammes08/DoViBakerVS/issues).

## Components

This plugin provides the following filters:

- [DoViBaker](#dovibaker): Bake a Dolby Vision stream to a PQ stream
- [DoViTonemap](#dovitonemap): Static or dynamic tonemapping of a PQ stream
- [DoViCubes](#dovicubes): Apply LUTs based on scene max content light level
- [DoViStatsFileLoader](#dovistatsfileloader): Load stats files for dynamic processing of non-DolbyVision PQ streams

Additional tools available from the [main DoViBaker repository](https://github.com/erazortt/DoViBaker):

- **DoViLutGen.exe**: Creates LUTs for converting PQ streams to HLG or SDR, or for converting HLG to SDR
- **DoViAnalyzer.exe**: Analyzes a given RPU.bin file to help decide if DolbyVision processing is needed

For an explanation of all the terminology used and the technical concepts behind, please consult the Red, Orange and Yellow Books of UHD Forum Guidelines: [https://ultrahdforum.org/guidelines/](https://ultrahdforum.org/guidelines/)

## Building

### Requirements
- CMake 3.28+
- VapourSynth SDK
- libdovi (from dovi_tool)
- C++20 compiler

### Build Instructions

```bash
git clone --recursive https://github.com/user/DoViBakerVS.git
cd DoViBakerVS
cmake -B build -DVAPOURSYNTH_SDK=/path/to/vapoursynth/sdk
cmake --build build --config Release
```

## DoViBaker

This plugin reads the Base Layer, Enhancement Layer and RPU data from a Dolby Vision stream to create a clip with all the processing indicated by these substreams baked into a PQ output stream.

### General Information

This plugin uses the metadata from an RPU file or from the stream itself to compose the DolbyVision HDR picture out of the Base Layer (BL) and Enhancement Layer (EL). Display Management (DM) metadata will not be processed by default. It is however possible to further process the clip using DM data by explicitly enabling [Trims](#trims) or by using [DoViTonemap](#dovitonemap) or [DoViCubes](#dovicubes).

### Source Plugins

Several source plugins can be used to feed DoViBaker. Choose based on performance testing on your system.

#### L-SMASH-Works

```python
import vapoursynth as vs
core = vs.core

bl = core.lsmas.LWLibavSource("clip.ts", stream_index=0)
el = core.lsmas.LWLibavSource("clip.ts", stream_index=1)
clip = core.dovi.Baker(bl, el)
```

#### BestSource

```python
import vapoursynth as vs
core = vs.core

bl = core.bs.VideoSource("clip.ts", track=0)
el = core.bs.VideoSource("clip.ts", track=1)
clip = core.dovi.Baker(bl, el)
```

#### DGDecNV (with external RPU)

When using DGDecNV, you need to extract the RPU data separately using dovi_tool:

1. Get dovi_tool: <https://github.com/quietvoid/dovi_tool/releases>
2. Extract the Base and Enhancement Layers separately from the initial profile 7 stream
3. Extract the RPU data from the Enhancement Layer using dovi_tool
4. Write a VapourSynth script:

```python
import vapoursynth as vs
core = vs.core

bl = core.dgdecodenv.DGSource("blclip.dgi")
el = core.dgdecodenv.DGSource("elclip.dgi")
clip = core.dovi.Baker(bl, el, rpu="RPU.bin")
```

### Output Stream

The default output is 16-bit RGB in PQ transfer with BT.2020 primaries. This means it is a 16-bit RGB stream employing Perceptual Quantization as the transfer function, with 12-bit effective color depth per component on the wide color gamut specified in BT.2100.

When `outYUV=1`, output is 16-bit YUV preserving the input subsampling with BT.2020 primaries and PQ transfer.

### Static Metadata for Encoding

When encoding HDR10 streams, you may need to add metadata manually. Using x265:

```text
--master-display "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50)"
--max-cll "1442,329"
```

The values for `L` in `master-display` are from frame properties `_dovi_static_master_display_max_luminance` (multiplied by 10000) and `_dovi_static_master_display_min_luminance`. The `G`, `B`, `R`, `WP` values shown are for Display P3 color gamut.

The `max-cll` values come from `_dovi_static_max_content_light_level` and `_dovi_static_max_avg_content_light_level`. Not all DolbyVision substreams carry these values; you may need to read them from the Base Layer using MediaInfo.

### Trims

It is possible to apply the trims available in the DolbyVision substream. Select which trim to apply using the `trimPq` argument and set `targetMaxNits` and `targetMinNits` as necessary. Note that only CM v2.9 processing is implemented, and most streams don't have optimized parameters, producing suboptimal results. This feature is experimental.

Typical trim targets:

- 100 nits: `trimPq=2081`
- 600 nits: `trimPq=2851`
- 1000 nits: `trimPq=3079`

For higher brightness targets (600+ nits), results are often better using [DoViTonemap](#dovitonemap) instead of trims.

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| bl | clip | required | Base Layer clip (YUV420P10 or YUV420P16) |
| el | clip | optional | Enhancement Layer clip (optional for profile 8.1) |
| rpu | string | "" | Path to RPU.bin file (if not embedded) |
| trimPq | int | 0 | Trim target PQ value (0 = disabled) |
| targetMaxNits | float | 100.0 | Target maximum brightness for trim |
| targetMinNits | float | 0.0 | Target minimum brightness for trim |
| qnd | int | 0 | Quick and dirty mode (faster but lower quality) |
| rgbProof | int | 0 | RGB proof mode for debugging |
| nlqProof | int | 0 | NLQ proof mode for debugging |
| outYUV | int | 0 | Output YUV instead of RGB (skips RGB conversion) |
| sourceProfile | int | 0 | Force source profile (0=auto, 7=FEL, 8=MEL) |

#### Parameter Constraints

**Mutually exclusive options:**

- `outYUV=1` cannot be combined with `qnd=1` (quick-and-dirty mode requires RGB output)
- `outYUV=1` cannot be combined with `rgbProof=1` (RGB proofing only applies to RGB output)
- `outYUV=1` requires the base layer to be chroma subsampled (YUV420)

### Usage Examples

**Basic usage with embedded RPU:**

```python
bl = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=0)
el = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=1)
clip = core.dovi.Baker(bl, el)
```

**Profile 8.1 (no enhancement layer):**

```python
bl = core.lsmas.LWLibavSource("profile81.ts")
clip = core.dovi.Baker(bl)
```

**YUV output for further processing:**

```python
bl = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=0)
el = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=1)
clip = core.dovi.Baker(bl, el, outYUV=1)
# Output is YUV420P16 with BT.2020 primaries and PQ transfer
```

**Override misdetected profile:**

```python
bl = core.lsmas.LWLibavSource("misidentified.ts", stream_index=0)
el = core.lsmas.LWLibavSource("misidentified.ts", stream_index=1)
clip = core.dovi.Baker(bl, el, sourceProfile=7)
```

**Apply 600-nit trim:**

```python
bl = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=0)
el = core.lsmas.LWLibavSource("dolbyvision.ts", stream_index=1)
clip = core.dovi.Baker(bl, el, trimPq=2851, targetMaxNits=600, targetMinNits=0.005)
```

### DoViBaker Frame Properties

The following frame properties are set:

- `_Matrix`: 0 for RGB output, 9 (BT.2020 NCL) for YUV output
- `_Primaries`: 9 (BT.2020)
- `_Transfer`: 16 (PQ/ST2084)
- `_ColorRange`: 0 for full range, 1 for limited range
- `_SceneChangePrev`: 1 for first frame in a scene
- `_dovi_dynamic_min_pq`: Min PQ value of current scene
- `_dovi_dynamic_max_pq`: Max PQ value of current scene
- `_dovi_dynamic_max_content_light_level`: Max nits of current scene
- `_dovi_static_max_pq`: Max PQ value of whole stream
- `_dovi_static_max_content_light_level`: Max nits of whole stream
- `_dovi_static_max_avg_content_light_level`: Maximum average nits
- `_dovi_static_master_display_max_luminance`: Mastering display max luminance in nits
- `_dovi_static_master_display_min_luminance`: Mastering display min luminance (x10000)

The static values are non-zero only when available in the DolbyVision substream.

## DoViTonemap

Processes tonemapping of HDR PQ streams to lower dynamic range targets. Implementation based on ITU-R BT.2408-7 Annex 5 (previously in ITU-R BT.2390), with an optional luminosity factor for linear brightness scaling.

Color space is preserved and not converted to narrower gamut. For conversions to HLG or SDR, additional processing is required using LUTs from [DoViLutGen.exe](https://github.com/erazortt/DoViBaker) (available in the main DoViBaker repository).

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| clip | clip | required | Input RGB clip |
| targetMaxNits | float | 1000.0 | Target maximum brightness |
| targetMinNits | float | 0.0 | Target minimum brightness |
| masterMaxNits | float | -1.0 | Source max brightness (-1 = read from frame props) |
| masterMinNits | float | -1.0 | Source min brightness (-1 = read from frame props) |
| lumScale | float | -1.0 | Luminosity scale factor (-1 = read from frame props, otherwise default 1.0) |
| kneeOffset | float | 0.75 | Tonemapping curve knee offset [0.5, 2.0] |
| normalizeOutput | int | 0 | Normalize output to full range |

### Parameter Details

- **masterMaxNits / masterMinNits**: Set the white and black brightness of the source. When both are `-1`, values are read from frame properties `_dovi_dynamic_max_pq` and `_dovi_dynamic_min_pq` (set by DoViBaker or DoViStatsFileLoader), enabling dynamic tonemapping.

- **lumScale**: Changes total brightness. Many HDR streams are darker than their SDR equivalents; typical values range from 1.0 to 5.0. This is especially important when converting to SDR. Set to `-1` to read from the `_dovi_dynamic_luminosity_scale` frame property.

- **kneeOffset**: Controls the size of the region where the tonemapping curve flattens. BT.2408 uses 0.5, which favors max brightness over highlight detail. The default of 0.75 provides better balance, especially for dynamic tonemapping.

- **normalizeOutput**: When enabled, normalizes output from `[targetMinNits, targetMaxNits]` to full range. Useful for intermediate results that will be further processed with LUTs, as it reduces rounding errors.

### Usage

```python
# Dynamic tonemapping using frame properties from DoViBaker
clip = core.dovi.Baker(bl, el)
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0)

# Static tonemapping with explicit master values
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0,
                          masterMaxNits=4000, masterMinNits=0)

# With brightness boost for dark sources
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0, lumScale=2.0)
```

If your source is PQ without a DolbyVision substream:

- Use static tonemapping by explicitly defining `masterMaxNits` and `masterMinNits`
- Or analyze the source to create a stats file for [DoViStatsFileLoader](#dovistatsfileloader), enabling dynamic tonemapping

![PQ Tonemapping function](EETF.png "PQ Tonemapping function")

### DoViTonemap Frame Properties

**Consumed** (when respective parameters are -1):
- `_dovi_dynamic_max_pq`
- `_dovi_dynamic_min_pq`
- `_dovi_dynamic_luminosity_scale`
- `_ColorRange`

**Set:**
- `_ColorRange`: 0 (output is always full range)

## DoViCubes

Applies LUT processing based on the `_dovi_dynamic_max_content_light_level` frame property. One LUT from a set is selected based on configurable thresholds.

The LUT processing is based on [timecube](https://github.com/sekrit-twc/timecube). This filter should not be used in conjunction with DoViTonemap; use timecube.Cube directly in that case.

### Usage

```python
clip = core.dovi.Baker(bl, el)
clip = core.dovi.Cubes(clip,
                        cubes="lut_1000.cube;lut_2000.cube;lut_4000.cube",
                        mclls="1010;2020",
                        cubes_basepath="C:/luts/")
```

This applies `lut_1000.cube` for scenes ≤1010 nits, `lut_2000.cube` for 1010-2020 nits, and `lut_4000.cube` for >2020 nits.

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| clip | clip | required | Input RGB clip |
| cubes | string | required | Semicolon-separated list of LUT files |
| mclls | string | "" | Semicolon-separated thresholds in nits |
| cubes_basepath | string | "" | Base path for LUT files |
| fullrange | int | 1 | Input is full range |

### DoViCubes Frame Properties

**Consumed:**

- `_dovi_dynamic_max_content_light_level`

## DoViStatsFileLoader

Loads stats files for dynamic processing of non-Dolby Vision PQ streams. This enables dynamic tonemapping with DoViTonemap or DoViCubes for regular HDR10 content.

### Usage

```python
clip = core.dovi.StatsFileLoader(clip, statsFile="stats.txt")
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0)
```

### Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| clip | clip | required | Input clip |
| statsFile | string | "" | Path to stats file |
| sceneCutsFile | string | "" | Optional alternative scene cuts file |

### Stats File Format

Each line of the stats file:

```text
<frame_number> <is_scene_end> <max_pq> <min_pq> [lum_scale]
```

- `frame_number`: Frame index (0-based)
- `is_scene_end`: 1 if this frame is the last in a scene, 0 otherwise
- `max_pq`: Maximum PQ value for this frame (0-4095)
- `min_pq`: Minimum PQ value for this frame (0-4095)
- `lum_scale`: Optional luminosity scale factor

Example:

```text
0 0 3200 100
1 0 3180 105
2 1 3150 110
3 0 2800 50
```

### Alternative Scene Cuts File Format

Each line contains the frame number of the first frame after a scene cut:

```text
<frame_number_of_first_frame_after_scene_cut>
```

### DoViStatsFileLoader Frame Properties

**Set:**

- `_SceneChangePrev`: 1 for first frame in a scene
- `_SceneChangeNext`: 1 for last frame in a scene
- `_dovi_dynamic_min_pq`: Min PQ value of current scene
- `_dovi_dynamic_max_pq`: Max PQ value of current scene
- `_dovi_dynamic_max_content_light_level`: Max nits of current scene
- `_dovi_dynamic_luminosity_scale`: Optional luminosity scale of current scene
- `_dovi_static_max_pq`: Max PQ value of whole stream
- `_dovi_static_max_content_light_level`: Max nits of whole stream

## Example Workflows

### Dolby Vision to HDR10 (1000 nits)

```python
import vapoursynth as vs
core = vs.core

bl = core.lsmas.LWLibavSource("clip.ts", stream_index=0)
el = core.lsmas.LWLibavSource("clip.ts", stream_index=1)

clip = core.dovi.Baker(bl, el)
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0)

# Convert to YUV for encoding
clip = core.resize.Bicubic(clip, format=vs.YUV420P10, matrix_s="2020ncl")
```

### Dolby Vision to HLG

Requires [DoViLutGen.exe](https://github.com/erazortt/DoViBaker) from the main DoViBaker repository to generate the LUT, and timecube (`vsrepo install timecube`):

```sh
DoViLutGen.exe pq2hlg.cube -s 65 -i 0 -o 0
```

```python
import vapoursynth as vs
core = vs.core

bl = core.lsmas.LWLibavSource("clip.ts", stream_index=0)
el = core.lsmas.LWLibavSource("clip.ts", stream_index=1)

clip = core.dovi.Baker(bl, el)
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0)
clip = core.timecube.Cube(clip, cube="pq2hlg.cube")
clip = core.resize.Bicubic(clip, format=vs.YUV420P10, matrix_s="2020ncl",
                            transfer_s="std-b67", primaries_s="2020")
```

### Dolby Vision to BT.709 SDR

Generate the LUT using [DoViLutGen.exe](https://github.com/erazortt/DoViBaker) with optional SDR look adjustments:

```sh
DoViLutGen.exe pq2sdr709.cube -s 65 -i 0 -o 3
```

The `-g` (gain) and `-c` (compression) options adjust the SDR look:

- Increase compression for a flatter, more traditional SDR look
- Increase gain to retain more HDR "pop"
- Default values (0.0) emulate typical SDR dynamic range

```python
import vapoursynth as vs
core = vs.core

bl = core.lsmas.LWLibavSource("clip.ts", stream_index=0)
el = core.lsmas.LWLibavSource("clip.ts", stream_index=1)

clip = core.dovi.Baker(bl, el)
# lumScale often needs to be 2.0+ for SDR conversions
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0, lumScale=2.5)
clip = core.timecube.Cube(clip, cube="pq2sdr709.cube")
clip = core.resize.Bicubic(clip, format=vs.YUV420P8, matrix_s="709",
                            transfer_s="709", primaries_s="709")
```

### Non-DolbyVision PQ to SDR (Static Tonemapping)

```python
import vapoursynth as vs
core = vs.core

clip = core.lsmas.LWLibavSource("hdr10.ts")
# Convert to RGB first
clip = core.resize.Bicubic(clip, format=vs.RGBP16, matrix_in_s="2020ncl",
                            transfer_in_s="st2084", primaries_in_s="2020")
# Static tonemap - specify source mastering info
clip = core.dovi.Tonemap(clip, targetMaxNits=1000, targetMinNits=0,
                          masterMaxNits=4000, masterMinNits=0, lumScale=2.0)
clip = core.timecube.Cube(clip, cube="pq2sdr709.cube")
clip = core.resize.Bicubic(clip, format=vs.YUV420P8, matrix_s="709",
                            transfer_s="709", primaries_s="709")
```

## License

See [LICENSE](LICENSE) file.

## Acknowledgments & Lineage

This is a VapourSynth port of [DoViBaker](https://github.com/erazortt/DoViBaker) by erazortt.

**Dependencies:**

- [libdovi](https://github.com/quietvoid/dovi_tool) by quietvoid - Dolby Vision RPU parsing
- [timecube](https://github.com/sekrit-twc/timecube) by sekrit-twc - LUT processing

**Features incorporated from community forks:**

- `sourceProfile` parameter from [Asd-g/DoViBaker](https://github.com/Asd-g/DoViBaker)
- `outYUV` parameter concept from [quietvoid/DoViBaker](https://github.com/quietvoid/DoViBaker) and Asd-g

---

*Development of this port was assisted by [Claude](https://claude.ai), an AI assistant by Anthropic.*
