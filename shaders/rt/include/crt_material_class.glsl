// THE MATERIAL CLASS A KERNEL IS BUILT FOR. A kernel that evaluates surface materials (every kernel
// 06 variant) includes this file directly, and the build compiles it once per class below, defining
// CRT_MATERIAL_CLASS_<ID> and writing <name>.comp.<id>.spv; CMakeLists.txt reads the list from the
// defined(...) lines of this file, as it reads the traversals from crt_trace.glsl.
// src/rt_kernels.h KernelVariant::perMaterialClass marks such a kernel, and the path schedule
// dispatches its basic build over CRT_QUEUE_SURFACE and its complex build over
// CRT_QUEUE_SURFACE_COMPLEX, basic first (PBR 4ed 15.3.9).
//
// The basic build evaluates the basic types alone, so it allocates registers for them alone. The
// complex build is pbrt's universal evaluator: it evaluates every type, so a material that is complex
// only because of what one of its parameters reads still has its type's code. A complex type's
// shader case is wrapped in #if CRT_MATERIAL_CLASS_EVALUATES_COMPLEX. None of the six original types
// is complex, so today both builds hold the same cases and the complex queue is always empty.
//
// Re-run cmake after editing this file, since the list of outputs is decided when it configures.
//
// Requires crt_common.glsl.

#if defined(CRT_MATERIAL_CLASS_BASIC)
#define CRT_MATERIAL_CLASS 0
#define CRT_MATERIAL_CLASS_EVALUATES_COMPLEX 0
#elif defined(CRT_MATERIAL_CLASS_COMPLEX)
#define CRT_MATERIAL_CLASS 1
#define CRT_MATERIAL_CLASS_EVALUATES_COMPLEX 1
#else
#error "a kernel that evaluates materials is compiled once per material class; build it through CMake, which defines CRT_MATERIAL_CLASS_<ID>"
#endif

// the surface queue this build drains
#define CRT_SURFACE_QUEUE (CRT_MATERIAL_CLASS == 0 ? CRT_QUEUE_SURFACE : CRT_QUEUE_SURFACE_COMPLEX)
