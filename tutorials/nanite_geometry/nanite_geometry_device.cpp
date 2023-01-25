// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "nanite_geometry_device.h"

#if defined(USE_GLFW)

/* include GLFW for window management */
#include <GLFW/glfw3.h>

/* include ImGUI */
#include "../common/imgui/imgui.h"
#include "../common/imgui/imgui_impl_glfw_gl2.h"

#endif


#include "../../kernels/rthwif/builder/gpu/lcgbp.h"

namespace embree {

  template<typename Ty>
    struct Averaged
  {
    Averaged (size_t N, double dt)
    : N(N), dt(dt) {}

    void add(double v)
    {
      values.push_front(std::make_pair(getSeconds(),v));
      if (values.size() > N) values.resize(N);
    }

    Ty get() const
    {
      if (values.size() == 0) return zero;
      double t_begin = values[0].first-dt;

      Ty sum(zero);
      size_t num(0);
      for (size_t i=0; i<values.size(); i++) {
        if (values[i].first >= t_begin) {
          sum += values[i].second;
          num++;
        }
      }
      if (num == 0) return 0;
      else return sum/Ty(num);
    }

    std::deque<std::pair<double,Ty>> values;
    size_t N;
    double dt;
  };


  
#define FEATURE_MASK                            \
  RTC_FEATURE_FLAG_TRIANGLE |                   \
  RTC_FEATURE_FLAG_INSTANCE
  
  RTCScene g_scene  = nullptr;
  TutorialData data;


  extern "C" RenderMode user_rendering_mode = RENDER_PRIMARY;
  extern "C" uint user_spp = 1;

  Averaged<double> avg_bvh_build_time(64,1.0);
  Averaged<double> avg_lod_selection_crack_fixing_time(64,1.0);
  


  // =========================================================================================================================================================
  // =========================================================================================================================================================
  // =========================================================================================================================================================
  
  static const uint LOD_LEVELS = 3;
  //static const uint NUM_TOTAL_QUAD_NODES_PER_RTC_LCG = (1-(1<<(2*LOD_LEVELS)))/(1-4);


  __forceinline Vec2f projectVertexToPlane(const Vec3f &p, const Vec3f &vx, const Vec3f &vy, const Vec3f &vz, const uint width, const uint height)
  {
    const Vec3f vn = cross(vx,vy);    
    const float distance = (float)dot(vn,vz) / (float)dot(vn,p);
    Vec3f pip = p * distance;
    if (distance < 0.0f)
      pip = vz;
    float a = dot((pip-vz),vx);
    float b = dot((pip-vz),vy);
    a = min(max(a,0.0f),(float)width);
    b = min(max(b,0.0f),(float)height);    
    return Vec2f(a,b);
  }
  
  __forceinline LODEdgeLevel getLODEdgeLevels(LCGBP &current,const ISPCCamera& camera, const uint width, const uint height)
  {
    const Vec3f v0 = current.patch.v0;
    const Vec3f v1 = current.patch.v1;
    const Vec3f v2 = current.patch.v2;
    const Vec3f v3 = current.patch.v3;

    const Vec3f vx = camera.xfm.l.vx;
    const Vec3f vy = camera.xfm.l.vy;
    const Vec3f vz = camera.xfm.l.vz;
    const Vec3f org = camera.xfm.p;

    const Vec2f p0 = projectVertexToPlane(v0-org,vx,vy,vz,width,height);
    const Vec2f p1 = projectVertexToPlane(v1-org,vx,vy,vz,width,height);
    const Vec2f p2 = projectVertexToPlane(v2-org,vx,vy,vz,width,height);
    const Vec2f p3 = projectVertexToPlane(v3-org,vx,vy,vz,width,height);

    const float f = 1.0/8.0f;
    const float d0 = length(p1-p0) * f;
    const float d1 = length(p2-p1) * f;
    const float d2 = length(p3-p2) * f;
    const float d3 = length(p0-p3) * f;

    
    int i0 = (int)floorf(d0 / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
    int i1 = (int)floorf(d1 / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
    int i2 = (int)floorf(d2 / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
    int i3 = (int)floorf(d3 / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
    
    i0 = min(max(0,i0),(int)LOD_LEVELS-1);
    i1 = min(max(0,i1),(int)LOD_LEVELS-1);
    i2 = min(max(0,i2),(int)LOD_LEVELS-1);
    i3 = min(max(0,i3),(int)LOD_LEVELS-1);
    
    LODEdgeLevel lod_levels(i0,i1,i2,i3);
    return lod_levels;
  }
  
  inline Vec3fa getVertex(const uint x, const uint y, const Vec3fa *const vtx, const uint grid_resX, const uint grid_resY)
  {
    const uint px = min(x,grid_resX-1);
    const uint py = min(y,grid_resY-1);    
    return vtx[py*grid_resX + px];
  }
  
  // ==============================================================================================
  // ==============================================================================================
  // ==============================================================================================
  
  struct __aligned(64) LCGBP_Scene {
    static const uint LOD_LEVELS = 3;

    uint numAllocatedLCGBP;
    uint numAllocatedLCGBPStates;
    
    uint numLCGBP;
    uint numCurrentLCGBPStates;    
    
    LCGBP *lcgbp;
    LCGBP_State *lcgbp_state;
    
    uint numCrackFixQuadNodes;
    
    RTCGeometry geometry;
    uint geomID;

    Texture* map_Kd;
    
    LCGBP_Scene(const uint maxNumLCGBP);

    void addGrid(const uint gridResX, const uint gridResY, const Vec3fa *const vtx);
  };
  
  LCGBP_Scene::LCGBP_Scene(const uint maxNumLCGBP)
  {
    numLCGBP = 0;
    numCurrentLCGBPStates = 0;    
    numAllocatedLCGBP = maxNumLCGBP; //((gridResX-1) / InitialSubGridRes) * ((gridResY-1) / InitialSubGridRes);
    numAllocatedLCGBPStates = (1<<(2*(LOD_LEVELS-1))) * maxNumLCGBP;
    lcgbp       = (LCGBP*)alignedUSMMalloc(sizeof(LCGBP)*numAllocatedLCGBP,64,EMBREE_USM_SHARED /*EmbreeUSMMode::EMBREE_DEVICE_READ_WRITE*/);
    lcgbp_state = (LCGBP_State*)alignedUSMMalloc(sizeof(LCGBP_State)*numAllocatedLCGBPStates,64,EMBREE_USM_SHARED/*EmbreeUSMMode::EMBREE_DEVICE_READ_WRITE*/);
    PRINT(numAllocatedLCGBP);
    PRINT(numAllocatedLCGBPStates);
  }

  void LCGBP_Scene::addGrid(const uint gridResX, const uint gridResY, const Vec3fa *const vtx)
  {
    double avg_error = 0.0;
    double max_error = 0.0;
    uint num_error = 0;

    PRINT(gridResX);
    PRINT(gridResY);
    
    for (int start_y=0;start_y+LCGBP::GRID_RES_QUAD<gridResY;start_y+=LCGBP::GRID_RES_QUAD)
      for (int start_x=0;start_x+LCGBP::GRID_RES_QUAD<gridResX;start_x+=LCGBP::GRID_RES_QUAD)
      {
        LCGBP &current = lcgbp[numLCGBP];

        const Vec3f v0 = getVertex(start_x,start_y,vtx,gridResX,gridResY);
        const Vec3f v1 = getVertex(start_x+LCGBP::GRID_RES_QUAD,start_y,vtx,gridResX,gridResY);
        const Vec3f v2 = getVertex(start_x+LCGBP::GRID_RES_QUAD,start_y+LCGBP::GRID_RES_QUAD,vtx,gridResX,gridResY);
        const Vec3f v3 = getVertex(start_x,start_y+LCGBP::GRID_RES_QUAD,vtx,gridResX,gridResY);

        const Vec2f u_range((float)start_x/(gridResX-1),(float)(start_x+LCGBP::GRID_RES_QUAD)/(gridResX-1));
        const Vec2f v_range((float)start_y/(gridResY-1),(float)(start_y+LCGBP::GRID_RES_QUAD)/(gridResY-1));
        
        new (&current) LCGBP(v0,v1,v2,v3,numLCGBP++,u_range,v_range);
        
        current.encode(start_x,start_y,vtx,gridResX,gridResY);
        
        for (int y=0;y<LCGBP::GRID_RES_VERTEX;y++)
        {
          for (int x=0;x<LCGBP::GRID_RES_VERTEX;x++)
          {
            const Vec3f org_v  = getVertex(start_x+x,start_y+y,vtx,gridResX,gridResY);
            const Vec3f new_v  = current.decode(x,y);
            const float error = length(new_v-org_v);
            if (error > 0.1)
            {
              PRINT5(x,y,LCGBP::as_uint(new_v.x),LCGBP::as_uint(new_v.y),LCGBP::as_uint(new_v.z));              
              //exit(0);
            }
            avg_error += (double)error;
            max_error = max(max_error,(double)error);
            num_error++;
          }
        }
      }
    PRINT2((float)(avg_error / num_error),max_error);
  }

  LCGBP_Scene *global_lcgbp_scene = nullptr;

  // ==============================================================================================
  // ==============================================================================================
  // ==============================================================================================
  
    
  
  void convertISPCGridMesh(ISPCGridMesh* grid, RTCScene scene, ISPCOBJMaterial *material)
  {
    uint numLCGBP = 0;
    
    /* --- count lcgbp --- */
    for (uint i=0;i<grid->numGrids;i++)
    {
      PRINT3(i,grid->grids[i].resX,grid->grids[i].resY);      
      const uint grid_resX = grid->grids[i].resX;
      const uint grid_resY = grid->grids[i].resY;
      const uint numInitialSubGrids = ((grid_resX-1) / LCGBP::GRID_RES_QUAD) * ((grid_resY-1) / LCGBP::GRID_RES_QUAD);
      PRINT(numInitialSubGrids);
      numLCGBP  += numInitialSubGrids;
    }
    PRINT(numLCGBP);

    /* --- allocate global LCGBP --- */
    global_lcgbp_scene = (LCGBP_Scene*)alignedUSMMalloc(sizeof(LCGBP_Scene),64);
    new (global_lcgbp_scene) LCGBP_Scene(numLCGBP);
    
    /* --- fill array of LCGBP --- */
    for (uint i=0;i<grid->numGrids;i++)
      global_lcgbp_scene->addGrid(grid->grids[i].resX,grid->grids[i].resY,grid->positions[0]);    

    global_lcgbp_scene->geometry = rtcNewGeometry (g_device, RTC_GEOMETRY_TYPE_LOSSY_COMPRESSED_GEOMETRY);
    rtcCommitGeometry(global_lcgbp_scene->geometry);
    global_lcgbp_scene->geomID = rtcAttachGeometry(scene,global_lcgbp_scene->geometry);
    //rtcReleaseGeometry(geom);
    global_lcgbp_scene->map_Kd = (Texture*)material->map_Kd;        
  }



  __forceinline Vec3fa getTexel3f(const Texture* texture, float s, float t)
  {
    int iu = (int)floor(s * (float)(texture->width));
    int iv = (int)floor(t * (float)(texture->height));    
    const int offset = (iv * texture->width + iu) * 4;
    unsigned char * txt = (unsigned char*)texture->data;
    const unsigned char  r = txt[offset+0];
    const unsigned char  g = txt[offset+1];
    const unsigned char  b = txt[offset+2];
    return Vec3fa(  (float)r * 1.0f/255.0f, (float)g * 1.0f/255.0f, (float)b * 1.0f/255.0f );
  }
  
  extern "C" ISPCScene* g_ispc_scene;

/* called by the C++ code for initialization */
  extern "C" void device_init (char* cfg)
  {
    TutorialData_Constructor(&data);
    /* create scene */
    data.g_scene = g_scene = rtcNewScene(g_device);
    rtcSetSceneBuildQuality(data.g_scene,RTC_BUILD_QUALITY_LOW);
    rtcSetSceneFlags(data.g_scene,RTC_SCENE_FLAG_DYNAMIC);

    PRINT(g_ispc_scene->numGeometries);
    PRINT(g_ispc_scene->numMaterials);
    
    for (unsigned int geomID=0; geomID<g_ispc_scene->numGeometries; geomID++)
    {
      ISPCGeometry* geometry = g_ispc_scene->geometries[geomID];
      if (geometry->type == GRID_MESH)
        convertISPCGridMesh((ISPCGridMesh*)geometry,data.g_scene, (ISPCOBJMaterial*)g_ispc_scene->materials[geomID]);
    }  
  
    /* update scene */
    rtcCommitScene (data.g_scene);  
  }


  Vec3fa randomColor(const int ID)
  {
    int r = ((ID+13)*17*23) & 255;
    int g = ((ID+15)*11*13) & 255;
    int b = ((ID+17)* 7*19) & 255;
    const float oneOver255f = 1.f/255.f;
    return Vec3fa(r*oneOver255f,g*oneOver255f,b*oneOver255f);
  }

/* task that renders a single screen tile */
  Vec3fa renderPixelPrimary(const TutorialData& data, float x, float y, const ISPCCamera& camera, const unsigned int width, const unsigned int height, LCGBP_Scene *grid)
  {
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    args.feature_mask = (RTCFeatureFlags) (FEATURE_MASK);
  
    /* initialize ray */
    Ray ray(Vec3fa(camera.xfm.p), Vec3fa(normalize(x*camera.xfm.l.vx + y*camera.xfm.l.vy + camera.xfm.l.vz)), 0.0f, inf);

    /* intersect ray with scene */
    rtcIntersect1(data.g_scene,RTCRayHit_(ray),&args);

    Vec3f color(1.0f,1.0f,1.0f);    
    if (ray.geomID == RTC_INVALID_GEOMETRY_ID)
      color = Vec3fa(0.0f);
    else
      color = Vec3fa( abs(dot(ray.dir,normalize(ray.Ng))) );
    return color;
  }

  Vec3fa renderPixelDebug(const TutorialData& data, float x, float y, const ISPCCamera& camera, const unsigned int width, const unsigned int height, LCGBP_Scene *lcgbp_scene, const RenderMode mode)
  {
    RTCIntersectArguments args;
    rtcInitIntersectArguments(&args);
    args.feature_mask = (RTCFeatureFlags) (FEATURE_MASK);
  
    /* initialize ray */
    Ray ray(Vec3fa(camera.xfm.p), Vec3fa(normalize(x*camera.xfm.l.vx + y*camera.xfm.l.vy + camera.xfm.l.vz)), 0.0f, inf);

    /* intersect ray with scene */
    rtcIntersect1(data.g_scene,RTCRayHit_(ray),&args);

    if (ray.geomID == RTC_INVALID_GEOMETRY_ID) return Vec3fa(0.0f);

    const uint localID = ray.primID & (((uint)1<<RTC_LOSSY_COMPRESSED_GRID_LOCAL_ID_SHIFT)-1);
    const uint primID = ray.primID >> RTC_LOSSY_COMPRESSED_GRID_LOCAL_ID_SHIFT;
    
    Vec3f color(1.0f,1.0f,1.0f);
    
    if (mode == RENDER_DEBUG_QUADS)
    {
      const float LINE_THRESHOLD = 0.1f;
      if (ray.u <= LINE_THRESHOLD ||
          ray.v <= LINE_THRESHOLD ||
          ray.u + ray.v <= LINE_THRESHOLD)
        color = Vec3fa(1.0f,0.0f,0.0f);      
    }
    else if (mode == RENDER_DEBUG_SUBGRIDS)
    {
      const uint gridID = lcgbp_scene->lcgbp_state[primID].lcgbp->ID;
      const uint subgridID = lcgbp_scene->lcgbp_state[primID].localID;    
      color = randomColor(gridID*(16+4+1)+subgridID);   
    }    
    else if (mode == RENDER_DEBUG_GRIDS)
    {
      const uint gridID = lcgbp_scene->lcgbp_state[primID].lcgbp->ID;      
      color = randomColor(gridID);   
    }
    else if (mode == RENDER_DEBUG_LOD)
    {
      const uint step = lcgbp_scene->lcgbp_state[primID].step; 
      if (step == 4)
        color = Vec3fa(0,0,1);
      else if (step == 2)
        color = Vec3fa(0,1,0);
      else if (step == 1)
        color = Vec3fa(1,0,0);            
    }
    else if (mode == RENDER_DEBUG_CRACK_FIXING)
    {
      const uint cracks_fixed = lcgbp_scene->lcgbp_state[primID].cracksFixed();
      if (cracks_fixed)
        color = Vec3fa(1,0,1);      
    }
    else if (mode == RENDER_DEBUG_UV)
    {
      const uint flip_uv = localID & 1;
      const uint localQuadID = localID>>1;
      const uint local_y = localQuadID /  RTC_LOSSY_COMPRESSED_GRID_QUAD_RES;
      const uint local_x = localQuadID %  RTC_LOSSY_COMPRESSED_GRID_QUAD_RES;

      const LCGBP_State &state = lcgbp_scene->lcgbp_state[primID];
      const LCGBP &current = *state.lcgbp;
      const uint start_x = state.start_x;
      const uint start_y = state.start_y;
      const uint end_x = state.start_x + state.step*8;
      const uint end_y = state.start_y + state.step*8;

      const float blend_start_u = (float)start_x / LCGBP::GRID_RES_QUAD;
      const float blend_end_u   = (float)  end_x / LCGBP::GRID_RES_QUAD;
      const float blend_start_v = (float)start_y / LCGBP::GRID_RES_QUAD;
      const float blend_end_v   = (float)  end_y / LCGBP::GRID_RES_QUAD;

      const Vec2f u_range(lerp(current.u_range.x,current.u_range.y,blend_start_u),lerp(current.u_range.x,current.u_range.y,blend_end_u));
      const Vec2f v_range(lerp(current.v_range.x,current.v_range.y,blend_start_v),lerp(current.v_range.x,current.v_range.y,blend_end_v));
      
      const float u = flip_uv ? 1-ray.u : ray.u;
      const float v = flip_uv ? 1-ray.v : ray.v;
      const float u_size = (u_range.y - u_range.x) * (1.0f / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
      const float v_size = (v_range.y - v_range.x) * (1.0f / RTC_LOSSY_COMPRESSED_GRID_QUAD_RES);
      const float u_start = u_range.x + u_size * (float)local_x;
      const float v_start = v_range.x + v_size * (float)local_y;      
      const float fu = u_start + u * u_size;
      const float fv = v_start + v * v_size;

      color = getTexel3f(lcgbp_scene->map_Kd,1-fu,fv);
      //color = Vec3fa(fu,fv,1.0f-fu-fv);
    }
    return Vec3fa(abs(dot(ray.dir,normalize(ray.Ng)))) * color;
  }
  

  void renderPixelStandard(const TutorialData& data,
                           int x, int y, 
                           int* pixels,
                           const unsigned int width,
                           const unsigned int height,
                           const float time,
                           const ISPCCamera& camera,
                           LCGBP_Scene *lcgbp_scene,
                           const RenderMode mode,
                           const uint spp)
  {
    RandomSampler sampler;

    Vec3fa color(0.0f);
    const float inv_spp = 1.0f / (float)spp;
    
    for (uint i=0;i<spp;i++)
    {
      RandomSampler_init(sampler, x, y, i);      
      float fx = x + RandomSampler_get1D(sampler);
      float fy = y + RandomSampler_get1D(sampler);
    
      /* calculate pixel color */
      if (mode == RENDER_PRIMARY)
        color += renderPixelPrimary(data, (float)fx,(float)fy,camera, width, height, lcgbp_scene);
      else
        color += renderPixelDebug(data, (float)fx,(float)fy,camera, width, height, lcgbp_scene, mode);
    }
    color *= inv_spp;
    
    /* write color to framebuffer */
    unsigned int r = (unsigned int) (255.0f * clamp(color.x,0.0f,1.0f));
    unsigned int g = (unsigned int) (255.0f * clamp(color.y,0.0f,1.0f));
    unsigned int b = (unsigned int) (255.0f * clamp(color.z,0.0f,1.0f));
    pixels[y*width+x] = (b << 16) + (g << 8) + r;
  }

  extern "C" void renderFrameStandard (int* pixels,
                                       const unsigned int width,
                                       const unsigned int height,
                                       const float time,
                                       const ISPCCamera& camera)
  {
    /* render all pixels */
#if defined(EMBREE_SYCL_TUTORIAL)
    RenderMode rendering_mode = user_rendering_mode;    
    LCGBP_Scene *lcgbp_scene = global_lcgbp_scene;
    uint spp = user_spp;
    TutorialData ldata = data;
    sycl::event event = global_gpu_queue->submit([=](sycl::handler& cgh){
                                                   const sycl::nd_range<2> nd_range = make_nd_range(height,width);
                                                   cgh.parallel_for(nd_range,[=](sycl::nd_item<2> item) {
                                                                               const unsigned int x = item.get_global_id(1); if (x >= width ) return;
                                                                               const unsigned int y = item.get_global_id(0); if (y >= height) return;
                                                                               renderPixelStandard(ldata,x,y,pixels,width,height,time,camera,lcgbp_scene,rendering_mode,spp);
                                                                             });
                                                 });
    global_gpu_queue->wait_and_throw();

    const auto t0 = event.template get_profiling_info<sycl::info::event_profiling::command_start>();
    const auto t1 = event.template get_profiling_info<sycl::info::event_profiling::command_end>();
    const double dt = (t1-t0)*1E-9;
    ((ISPCCamera*)&camera)->render_time = dt;  
#endif
  }

  __forceinline size_t alignTo(const uint size, const uint alignment)
  {
    return ((size+alignment-1)/alignment)*alignment;
  }

  __forceinline void waitOnQueueAndCatchException(sycl::queue &gpu_queue)
  {
    try {
      gpu_queue.wait_and_throw();
    } catch (sycl::exception const& e) {
      std::cout << "Caught synchronous SYCL exception:\n"
                << e.what() << std::endl;
      FATAL("SYCL Exception");     
    }      
  }

  __forceinline void waitOnEventAndCatchException(sycl::event &event)
  {
    try {
      event.wait_and_throw();
    } catch (sycl::exception const& e) {
      std::cout << "Caught synchronous SYCL exception:\n"
                << e.what() << std::endl;
      FATAL("SYCL Exception");     
    }      
  }

  __forceinline float getDeviceExecutionTiming(sycl::event &queue_event)
  {
    const auto t0 = queue_event.template get_profiling_info<sycl::info::event_profiling::command_start>();
    const auto t1 = queue_event.template get_profiling_info<sycl::info::event_profiling::command_end>();
    return (float)((t1-t0)*1E-6);      
  }

  template<typename T>
  static __forceinline uint atomic_add_global(T *dest, const T count=1)
  {
    sycl::atomic_ref<T, sycl::memory_order::relaxed, sycl::memory_scope::device,sycl::access::address_space::global_space> counter(*dest);        
    return counter.fetch_add(count);      
  }


  extern "C" void device_gui()
  {
    ImGui::Text("SPP: %d",user_spp);    
    ImGui::Text("BVH Build Time: %4.4f ms",avg_bvh_build_time.get());
    ImGui::Text("numGrids9x9:   %d (out of %d)",global_lcgbp_scene->numCurrentLCGBPStates,global_lcgbp_scene->numLCGBP*(1<<(LOD_LEVELS+1)));
    ImGui::Text("numGrids33x33: %d ",global_lcgbp_scene->numLCGBP);    
  }
  
  
/* called by the C++ code to render */
  extern "C" void device_render (int* pixels,
                                 const unsigned int width,
                                 const unsigned int height,
                                 const float time,
                                 const ISPCCamera& camera)
  {
#if defined(EMBREE_SYCL_TUTORIAL)
    
    LCGBP_Scene *local_lcgbp_scene = global_lcgbp_scene;
    sycl::event init_event =  global_gpu_queue->submit([&](sycl::handler &cgh) {
                                                         cgh.single_task([=]() {
                                                                           local_lcgbp_scene->numCurrentLCGBPStates = 0;
                                                                         });
                                                       });

    waitOnEventAndCatchException(init_event);
    
    const uint wgSize = 64;
    const uint numLCGBP = local_lcgbp_scene->numLCGBP;
    const sycl::nd_range<1> nd_range1(alignTo(numLCGBP,wgSize),sycl::range<1>(wgSize));              
    sycl::event compute_lod_event = global_gpu_queue->submit([=](sycl::handler& cgh){
                                                               cgh.depends_on(init_event);                                                   
                                                               cgh.parallel_for(nd_range1,[=](sycl::nd_item<1> item) {
                                                                                           const uint i = item.get_global_id(0);
                                                                                           if (i < numLCGBP)
                                                                                           {
                                                                                             LODEdgeLevel edgeLevels = getLODEdgeLevels(local_lcgbp_scene->lcgbp[i],camera,width,height);

#if 0                                                                                             
                                                                                             if (i == 0)
                                                                                               edgeLevels = LODEdgeLevel(0);
                                                                                             else if (i == 1)
                                                                                               edgeLevels = LODEdgeLevel(1,1,1,0);
                                                                                             else if (i == 2)
                                                                                               edgeLevels = LODEdgeLevel(0,2,2,2);
                                                                                             else if (i == 3)
                                                                                               edgeLevels = LODEdgeLevel(1,2,2,2);
#endif                                                                                             
                                                                                                    
                                                                                             
                                                                                             const uint lod_level = edgeLevels.level();
                                                                                             
                                                                                             const uint numGrids9x9 = 1<<(2*lod_level);
                                                                                             //const uint offset = ((1<<(2*lod_level))-1)/(4-1);
                                                                                             const uint offset = atomic_add_global(&local_lcgbp_scene->numCurrentLCGBPStates,numGrids9x9);
                                                                                             uint index = 0;
                                                                                             if (lod_level == 0)
                                                                                             {
                                                                                               local_lcgbp_scene->lcgbp_state[offset+index] = LCGBP_State(&local_lcgbp_scene->lcgbp[i],0,0,4,index,edgeLevels);
                                                                                               index++;
                                                                                             }
                                                                                             else if (lod_level == 1)
                                                                                             {
                                                                                               for (uint y=0;y<2;y++)
                                                                                                 for (uint x=0;x<2;x++)
                                                                                                 {
                                                                                                   local_lcgbp_scene->lcgbp_state[offset+index] = LCGBP_State(&local_lcgbp_scene->lcgbp[i],x*16,y*16,2,index,edgeLevels);
                                                                                                   index++;
                                                                                                 }
                                                                                             }
                                                                                             else
                                                                                             {
                                                                                               for (uint y=0;y<4;y++)
                                                                                                 for (uint x=0;x<4;x++)
                                                                                                 {
                                                                                                   local_lcgbp_scene->lcgbp_state[offset+index] = LCGBP_State(&local_lcgbp_scene->lcgbp[i],x*8,y*8,1,index,edgeLevels);
                                                                                                   index++;
                                                                                                 }
                                                                                             }
                                                                                           }
                                                                                           
                                                                                         });
                                                             });
    waitOnEventAndCatchException(compute_lod_event);

    double t0 = getSeconds();
    
    rtcSetGeometryUserData(local_lcgbp_scene->geometry,local_lcgbp_scene->lcgbp_state);
    rtcSetLossyCompressedGeometryPrimitiveCount(local_lcgbp_scene->geometry,local_lcgbp_scene->numCurrentLCGBPStates);
    rtcCommitGeometry(local_lcgbp_scene->geometry);
    
    /* commit changes to scene */
    rtcCommitScene (data.g_scene);

    double dt0 = (getSeconds()-t0)*1000.0;
    avg_bvh_build_time.add(dt0);
#endif
  }

/* called by the C++ code for cleanup */
  extern "C" void device_cleanup ()
  {
    TutorialData_Destructor(&data);
  }

} // namespace embree
