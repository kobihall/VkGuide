// THE TRAVERSAL A KERNEL IS BUILT FOR. A kernel that traces rays includes this file, not a
// strategy file, and gets traceScene() and occluded() (crt_traverse.glsl states the contract) from
// whichever traversal it was compiled for.
//
// The build compiles every .comp that includes this file once per traversal below, defining
// CRT_TRAVERSAL_<ID> and writing <name>.comp.<id>.spv; CMakeLists.txt reads the list from the
// defined(...) lines of this file, so they are the one place it is written. src/rt_kernels.cpp
// TraversalVariant names each id, and the renderer loads the .spv of the traversal selected.
//
// ADDING A TRAVERSAL. A strategy file under the contract in crt_traverse.glsl, one branch here, and
// one TraversalVariant row. Every kernel that traces, in every integrator, is then built for it.
// Re-run cmake after editing this file, since the list of outputs is decided when it configures.
//
// Requires crt_common.glsl.

#if defined(CRT_TRAVERSAL_BVH_BINARY)
#include "crt_bvh.glsl"
#elif defined(CRT_TRAVERSAL_BVH_CWBVH)
#define CRT_BVH_CWBVH 1
#include "crt_bvh.glsl"
#elif defined(CRT_TRAVERSAL_LINEAR)
#include "crt_linear.glsl"
#else
#error "a kernel that traces is compiled once per traversal; build it through CMake, which defines CRT_TRAVERSAL_<ID>"
#endif
