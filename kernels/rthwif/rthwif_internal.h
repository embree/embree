// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

typedef __attribute__((opencl_global)) struct rtglobals_t_* rtglobals_t;
typedef __attribute__((opencl_private)) struct rtfence_t_* rtfence_t;

#ifdef __SYCL_DEVICE_ONLY__

SYCL_EXTERNAL extern "C" void *intel_get_rt_stack(rtglobals_t rt_dispatch_globals);
SYCL_EXTERNAL extern "C" void *intel_get_thread_btd_stack(rtglobals_t rt_dispatch_globals);
SYCL_EXTERNAL extern "C" void *intel_get_global_btd_stack(rtglobals_t rt_dispatch_globals);
SYCL_EXTERNAL extern "C" rtfence_t intel_dispatch_trace_ray_query(rtglobals_t rt_dispatch_globals, unsigned int bvh_level, unsigned int traceRayCtrl);
SYCL_EXTERNAL extern "C" void intel_rt_sync(rtfence_t fence);

#else

inline void *intel_get_rt_stack(rtglobals_t rt_dispatch_globals) { return nullptr; }
inline void *intel_get_thread_btd_stack(rtglobals_t rt_dispatch_globals) { return nullptr; }
inline void *intel_get_global_btd_stack(rtglobals_t rt_dispatch_globals) { return nullptr; }
inline rtfence_t intel_dispatch_trace_ray_query(rtglobals_t rt_dispatch_globals, unsigned int bvh_level, unsigned int traceRayCtrl) { return nullptr; }
inline void intel_rt_sync(rtfence_t fence) {}

#endif

enum NodeType
{
  NODE_TYPE_MIXED = 0x0,        // identifies a mixed internal node where each child can have a different type
  NODE_TYPE_INTERNAL = 0x0,     // internal BVH node with 6 children
  NODE_TYPE_INSTANCE = 0x1,     // instance leaf 
  NODE_TYPE_PROCEDURAL = 0x3,   // procedural leaf
  NODE_TYPE_QUAD = 0x4,         // quad leaf
  NODE_TYPE_INVALID = 0x7       // indicates invalid node
};

struct __attribute__ ((packed,aligned(32))) MemRay
{
  // 32 B  
  float org[3];
  float dir[3];
  float tnear;
  float tfar;
  
  // 32 B
  struct { // FIXME: removing these anonymous structs triggers IGC bug
    uint64_t rootNodePtr : 48;  // root node to start traversal at
    uint64_t rayFlags : 16;     // ray flags (see RayFlag structure)
  };

  struct {
    uint64_t hitGroupSRBasePtr : 48; // base of hit group shader record array (16-bytes alignment)
    uint64_t hitGroupSRStride : 16;  // stride of hit group shader record array (16-bytes alignment)
  };

  struct {
    uint64_t missSRPtr : 48;  // pointer to miss shader record to invoke on a miss (8-bytes alignment)
    uint64_t pad0 : 8;        // padding byte (has to be zero)
    uint64_t shaderIndexMultiplier : 8; // shader index multiplier
  };

  struct {
    uint64_t instLeafPtr : 48;  // the pointer to instance leaf in case we traverse an instance (64-bytes alignment)
    uint64_t rayMask : 8;       // ray mask used for ray masking
    uint64_t pad1 : 8;          // padding byte (has to be zero)
  };
};

struct __attribute__ ((packed,aligned(32))) MemHit 
{
  inline void* getPrimLeafPtr() {
    return sycl::global_ptr<void>((void*)(uint64_t(primLeafPtr)*64)).get();
  }

  inline void* getInstanceLeafPtr() {
    return sycl::global_ptr<void>((void*)(uint64_t(instLeafPtr)*64)).get();
  }
  
  float    t;                   // hit distance of current hit (or initial traversal distance)
  float    u,v;                 // barycentric hit coordinates

  union {
    struct {
      uint32_t primIndexDelta  : 16; // prim index delta for compressed meshlets and quads
      uint32_t valid           : 1; // set if there is a hit
      uint32_t leafType        : 3; // type of node primLeafPtr is pointing to
      uint32_t primLeafIndex   : 4; // index of the hit primitive inside the leaf
      uint32_t bvhLevel        : 3; // the instancing level at which the hit occured
      uint32_t frontFace       : 1; // whether we hit the front-facing side of a triangle (also used to pass opaque flag when calling intersection shaders)
      uint32_t done            : 1; // used in sync mode to indicate that traversal is done
      uint32_t pad0            : 3; // unused bits
    };
    uint32_t data;
  };
    
  struct { // FIXME: removing these anonymous structs triggers IGC bug
    uint64_t primLeafPtr     : 42; // pointer to BVH leaf node (multiple of 64 bytes)
    uint64_t hitGroupRecPtr0 : 22; // LSB of hit group record of the hit triangle (multiple of 16 bytes)
  };

  struct {
    uint64_t instLeafPtr     : 42; // pointer to BVH instance leaf node (in multiple of 64 bytes)
    uint64_t hitGroupRecPtr1 : 22; // MSB of hit group record of the hit triangle (multiple of 16 bytes)
  };

  void clear(bool _done, bool _valid) {
    //*(sycl::int8*) this = sycl::int8(0x7F800000 /* INFINITY */, 0, 0, (_done ? 0x10000000 : 0) | (_valid ? 0x10000), 0, 0, 0, 0);
    t = u = v = 0.0f;
    data = 0;
    done = _done ? 1 : 0;
    valid = _valid ? 1 : 0;    
  }
};

struct __attribute__ ((packed,aligned(64))) RTStack
{
  union {
    struct {
      struct MemHit committedHit;    // stores committed hit
      struct MemHit potentialHit;    // stores potential hit that is passed to any hit shader
    };
    struct MemHit hit[2]; // committedHit, potentialHit
  };
  struct MemRay ray[2];
  char travStack[32*2];
};

struct __attribute__ ((packed)) HWAccel
{
  uint64_t rootNodeOffset;        // root node offset (and node type in lower 3 bits)
  float bounds[2][3];             // bounding box of the BVH
  uint32_t reserved0[8];
  uint32_t numTimeSegments;
  uint32_t reserved1[13];
  uint64_t dispatchGlobalsPtr;
};

struct  __attribute__ ((packed,aligned(8))) PrimLeafDesc 
{
  struct {
    uint32_t shaderIndex : 24;    // shader index used for shader record calculations
    uint32_t geomMask    : 8;     // geometry mask used for ray masking
  };

  struct {
    uint32_t geomIndex   : 29; // the geometry index specifies the n'th geometry of the scene
    uint32_t type        : 1;  // enable/disable culling for procedurals and instances
    uint32_t geomFlags   : 2;  // geometry flags of this geometry
  };
};

struct __attribute__ ((packed,aligned(64))) QuadLeaf
{
  struct PrimLeafDesc leafDesc;
  unsigned int primIndex0;
  struct {
    uint32_t primIndex1Delta : 16;  // delta encoded primitive index of second triangle
    uint32_t j0              : 2;   // specifies first vertex of second triangle
    uint32_t j1              : 2;   // specified second vertex of second triangle
    uint32_t j2              : 2;   // specified third vertex of second triangle    
    uint32_t last            : 1;   // true if the second triangle is the last triangle in a leaf list
    uint32_t pad             : 9;   // unused bits
  };
  float v[4][3]; 
};

struct __attribute__ ((packed,aligned(64))) ProceduralLeaf
{
  static const constexpr uint32_t N = 13;
  
  struct PrimLeafDesc leafDesc; // leaf header identifying the geometry
  struct {
    uint32_t numPrimitives : 4; // number of stored primitives
    uint32_t pad           : 32-4-N;
    uint32_t last          : N; // bit vector with a last bit per primitive
  };
  uint32_t _primIndex[N]; // primitive indices of all primitives stored inside the leaf
};

struct __attribute__ ((packed,aligned(64))) InstanceLeaf
{
  /* first 64 bytes accessed during traversal by hardware */
  struct Part0
  {
  public:
    struct {
      uint32_t shaderIndex : 24;  // shader index used to calculate instancing shader in case of software instancing
      uint32_t geomMask : 8;      // geometry mask used for ray masking
    };

    struct {
      uint32_t instanceContributionToHitGroupIndex : 24;
      uint32_t pad0 : 5;
      
      /* the following two entries are only used for procedural instances */
      uint32_t type : 1; // enables/disables opaque culling
      uint32_t geomFlags : 2; // unused for instances
    };

    struct {
      uint64_t startNodePtr : 48;  // start node where to continue traversal of the instanced object
      uint64_t instFlags : 8;      // flags for the instance (see InstanceFlags)
      uint64_t pad1 : 8;           // unused bits
    };
    
    float world2obj_vx[3];   // 1st column of Worl2Obj transform
    float world2obj_vy[3];   // 2nd column of Worl2Obj transform
    float world2obj_vz[3];   // 3rd column of Worl2Obj transform
    float obj2world_p[3];    // translation of Obj2World transform (on purpose in first 64 bytes)
  } part0;
  
  /* second 64 bytes accessed during shading */
  struct Part1
  {
    struct {
      uint64_t bvhPtr : 48;   // pointer to BVH where start node belongs too
      uint64_t pad : 16;      // unused bits
    };

    uint32_t instanceID;    // user defined value per DXR spec
    uint32_t instanceIndex; // geometry index of the instance (n'th geometry in scene)
    
    float obj2world_vx[3];   // 1st column of Obj2World transform
    float obj2world_vy[3];   // 2nd column of Obj2World transform
    float obj2world_vz[3];   // 3rd column of Obj2World transform
    float world2obj_p[3];    // translation of World2Obj transform
  } part1;
};