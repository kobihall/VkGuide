#pragma once

#include <cstddef>

// The checks of docs/plans/completed/pbrt-robust-intersections.md, on the CPU mirror of the tracer
// (src/robust.h, the watertight test and slabs in src/bvh_layout.*, the hit points in
// src/bvh_scene.*, the reprojection in src/shape.*):
//  - nextFloatUp()/nextFloatDown() against std::nextafter;
//  - the crack test: rays aimed at the shared edges and vertices of a tessellated sphere. Moller-
//    Trumbore may leak through some; the watertight test must leak through none;
//  - the error bounds of triangle and shape hit points, against a double-precision reference;
//  - self-intersection: a ray spawned from a hit point by offsetRayOrigin() must not hit the surface
//    it left, whichever side it leaves through, on meshes and on every shape, at three scales.
// Prints a table per check and returns the number of problems found.
int checkRobustIntersections(size_t rayCount);
