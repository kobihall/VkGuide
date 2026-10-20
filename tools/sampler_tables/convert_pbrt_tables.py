#!/usr/bin/env python3
"""Converts pbrt-v4's sampler tables from C++ source to the binary files rt_sampler loads.

    python3 tools/sampler_tables/convert_pbrt_tables.py <pbrt-v4>/src/pbrt/util assets/sampler

Reads sobolmatrices.cpp, pmj02tables.cpp and bluenoise.cpp. Writes, little-endian:

    sobol_matrices.bin  "SOBL", u32 dimensions (1024), u32 columns (52), u32 rows (25), then
                        u32 SobolMatrices32[dimensions * columns],
                        u64 VdCSobolMatrices[rows][columns], u64 VdCSobolMatricesInv[rows][columns]
    pmj02bn.bin         "PMJ2", u32 sets (5), u32 samples (65536), then u32 [sets][samples][2]
    blue_noise.bin      "BLUE", u32 textures (48), u32 resolution (128), then u16 [textures][x][y]

The licenses of the three sources are in assets/sampler/NOTICE.md.
"""
import re
import struct
import sys
from pathlib import Path


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def numbers(text):
    return [int(token, 0) for token in re.findall(r"0x[0-9a-fA-F]+|\d+", re.sub(r"(?<=[0-9a-fA-F])ULL", "", text))]


def array_body(text, name):
    start = text.index(name)
    start = text.index("=", start)
    depth, end = 0, start
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                end = i
                break
    return text[start:end + 1]


def main():
    source = Path(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    sobol = strip_comments((source / "sobolmatrices.cpp").read_text())
    dims, cols = 1024, 52
    matrices = numbers(array_body(sobol, "SobolMatrices32[NSobolDimensions"))
    assert len(matrices) == dims * cols, len(matrices)

    def rows(name):
        body = array_body(sobol, name)
        # one inner brace pair per row
        result = []
        for row in re.findall(r"\{([^{}]*)\}", body):
            values = numbers(row)
            assert len(values) <= cols
            result.append(values + [0] * (cols - len(values)))
        return result

    vdc = rows("VdCSobolMatrices[][SobolMatrixSize]")
    vdc_inv = rows("VdCSobolMatricesInv[][SobolMatrixSize]")
    # pbrt's inverse table has one row more than its forward table; SobolIntervalToIndex needs row m - 1 of both
    vdc_inv = vdc_inv[:len(vdc)]
    assert len(vdc) == len(vdc_inv)
    with open(out / "sobol_matrices.bin", "wb") as f:
        f.write(b"SOBL" + struct.pack("<III", dims, cols, len(vdc)))
        f.write(struct.pack(f"<{len(matrices)}I", *matrices))
        for table in (vdc, vdc_inv):
            for row in table:
                f.write(struct.pack(f"<{cols}Q", *row))

    pmj = numbers(array_body(strip_comments((source / "pmj02tables.cpp").read_text()), "pmj02bnSamples[nPMJ02bnSets"))
    sets, samples = 5, 65536
    assert len(pmj) == sets * samples * 2, len(pmj)
    with open(out / "pmj02bn.bin", "wb") as f:
        f.write(b"PMJ2" + struct.pack("<II", sets, samples))
        f.write(struct.pack(f"<{len(pmj)}I", *pmj))

    blue = numbers(array_body(strip_comments((source / "bluenoise.cpp").read_text()), "BlueNoiseTextures"))
    textures, resolution = 48, 128
    assert len(blue) == textures * resolution * resolution, len(blue)
    with open(out / "blue_noise.bin", "wb") as f:
        f.write(b"BLUE" + struct.pack("<II", textures, resolution))
        f.write(struct.pack(f"<{len(blue)}H", *blue))

    print(f"sobol: {len(vdc)} VdC rows; pmj02bn: {len(pmj) // 2} points; blue noise: {textures} textures")


main()
