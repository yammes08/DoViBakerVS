# Changelog

All notable changes to DoViBakerVS will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## 0.1.1 (Pre-release)

### Fixed

- Static linking of libdovi to eliminate runtime dependency on dovi.dll
- Resolves DLL load error 126 ("file or dependency missing")

## 0.1.0 (Pre-release)

Initial release of DoViBakerVS - a VapourSynth port of DoViBaker. This is an early release with limited testing.

### Features

- **DoViBaker**: Bake Dolby Vision streams to PQ output
  - Support for profiles 5, 7, and 8
  - `sourceProfile` parameter for manual profile override
  - `outYUV` parameter for YUV output mode
- **DoViTonemap**: HDR PQ tonemapping based on ITU-R BT.2408-7
  - Dynamic and static tonemapping modes
  - Configurable knee offset (default 0.75)
  - Luminosity scale support
- **DoViCubes**: Multi-LUT processing based on scene brightness
- **DoViStatsFileLoader**: Stats file loading for non-DolbyVision content

### Dependencies

- libdovi for Dolby Vision RPU parsing
- timecube for LUT processing
