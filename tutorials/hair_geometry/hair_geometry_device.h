// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "../common/tutorial/tutorial_device.h"
#include "../common/tutorial/scene_device.h"

namespace embree {

extern "C" ISPCScene* g_ispc_scene;
extern "C" Vec3fa g_dirlight_direction;
extern "C" Vec3fa g_dirlight_intensity;
extern "C" Vec3fa g_ambient_intensity;

struct TutorialData
{
  RTCScene scene;
  ISPCScene* ispc_scene;
  Vec3f dirlight_direction;
  Vec3f dirlight_intensity;
  Vec3f ambient_intensity;
  
  /* accumulation buffer */
  Vec3ff* accu;
  unsigned int accu_width;
  unsigned int accu_height;
  unsigned int accu_count;
  Vec3f accu_vx;
  Vec3f accu_vy;
  Vec3f accu_vz;
  Vec3f accu_p;
  bool g_subdiv_mode;

  /* hair material */
  Vec3f hair_K;
  Vec3f hair_dK;
  Vec3f hair_Kr;    //!< reflectivity of hair
  Vec3f hair_Kt;    //!< transparency of hair

  RTCFilterFunctionN occlusionFilter;
};

#if __SYCL_COMPILER_VERSION >= 20210801
}
namespace sycl {
  template<> struct is_device_copyable<embree::TutorialData> : std::true_type {};
  template<> struct is_device_copyable<const embree::TutorialData> : std::true_type {};
}
namespace embree {
#endif

void TutorialData_Constructor(TutorialData* This)
{
  This->scene = nullptr;
  This->ispc_scene = g_ispc_scene;
  This->dirlight_direction = Vec3f(g_dirlight_direction);
  This->dirlight_intensity = Vec3f(g_dirlight_intensity);
  This->ambient_intensity = Vec3f(g_ambient_intensity);
  This->accu = nullptr;
  This->accu_width = 0;
  This->accu_height = 0;
  This->accu_count = 0;
  This->g_subdiv_mode = false;
  
  /* initialize last seen camera */
  This->accu_vx = Vec3f(0.0f);
  This->accu_vy = Vec3f(0.0f);
  This->accu_vz = Vec3f(0.0f);
  This->accu_p  = Vec3f(0.0f);

  /* initialize hair colors */
  This->hair_K  = Vec3f(0.8f,0.57f,0.32f);
  This->hair_dK = Vec3f(0.1f,0.12f,0.08f);
  This->hair_Kr = 0.2f*This->hair_K;    //!< reflectivity of hair
  This->hair_Kt = 0.8f*This->hair_K;    //!< transparency of hair
  This->occlusionFilter = nullptr;
}

void TutorialData_Destructor(TutorialData* This)
{
  rtcReleaseScene (This->scene); This->scene = nullptr;
  alignedUSMFree(This->accu); This->accu = nullptr;
  This->accu_width = 0;
  This->accu_height = 0;
  This->accu_count = 0;
}

} // namespace embree
