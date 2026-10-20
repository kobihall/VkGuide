# Sampler tables

`tools/sampler_tables/convert_pbrt_tables.py` writes these files from the sources of
[pbrt-v4](https://github.com/mmp/pbrt-v4), and `src/sampler.cpp` reads them. The file formats are in
the script's header comment.

- `sobol_matrices.bin` holds `SobolMatrices32`, `VdCSobolMatrices` and `VdCSobolMatricesInv` from
  pbrt-v4's `src/pbrt/util/sobolmatrices.cpp`: Copyright (c) 2012 Leonhard Gruenschloss, MIT
  license, after S. Joe and F. Y. Kuo, *Constructing Sobol sequences with better two-dimensional
  projections*, SIAM J. Sci. Comput. 30, 2635–2654 (2008). pbrt is Copyright (c) 1998-2020 Matt
  Pharr, Wenzel Jakob and Greg Humphreys, Apache License 2.0.
- `pmj02bn.bin` holds `pmj02bnSamples` from pbrt-v4's `src/pbrt/util/pmj02tables.cpp`, Apache
  License 2.0. The points follow Christensen, Kensler and Kilpatrick, *Progressive multi-jittered
  sample sequences* (2018).
- `blue_noise.bin` holds `BlueNoiseTextures` from pbrt-v4's `src/pbrt/util/bluenoise.cpp`, Apache
  License 2.0, converted by pbrt from Christoph Peters's free blue-noise textures
  (<http://momentsingraphics.de/BlueNoise.html>), released under CC0.
