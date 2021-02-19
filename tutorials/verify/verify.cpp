// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#define _CRT_SECURE_NO_WARNINGS

#include "verify.h"
#include "../common/scenegraph/scenegraph.h"
#include "../common/scenegraph/geometry_creation.h"
#include "../common/math/closest_point.h"
#include "../../common/algorithms/parallel_for.h"
#include "../../common/simd/simd.h"
#include "../../kernels/common/context.h"
#include "../../kernels/common/geometry.h"
#include "../../kernels/common/scene.h"
#include <regex>
#include <stack>

#define random  use_random_function_of_test // do use random_int() and random_float() from Test class
#define drand48 use_random_function_of_test // do use random_int() and random_float() from Test class

#define DEFAULT_STACK_SIZE 4*1024*1024
#define TEXT_ALIGN 85
 
namespace embree
{
  extern "C" {
    RTCDevice g_device = nullptr; // as scene graph needs global device
  }
  
  /* error reporting function */
  void errorHandler(void* userPtr, const RTCError code, const char* str = nullptr)
  {
    if (code == RTC_ERROR_NONE)
      return;
    
    std::string descr = str ? ": " + std::string(str) : "";
    switch (code) {
    case RTC_ERROR_UNKNOWN          : throw std::runtime_error("RTC_ERROR_UNKNOWN"+descr);
    case RTC_ERROR_INVALID_ARGUMENT : throw std::runtime_error("RTC_ERROR_INVALID_ARGUMENT"+descr); break;
    case RTC_ERROR_INVALID_OPERATION: throw std::runtime_error("RTC_ERROR_INVALID_OPERATION"+descr); break;
    case RTC_ERROR_OUT_OF_MEMORY    : throw std::runtime_error("RTC_ERROR_OUT_OF_MEMORY"+descr); break;
    case RTC_ERROR_UNSUPPORTED_CPU  : throw std::runtime_error("RTC_ERROR_UNSUPPORTED_CPU"+descr); break;
    case RTC_ERROR_CANCELLED        : throw std::runtime_error("RTC_ERROR_CANCELLED"+descr); break;
    default                         : throw std::runtime_error("invalid error code"+descr); break;
    }
  }

  bool hasISA(const int isa) 
  {
    int cpu_features = getCPUFeatures();
    return (cpu_features & isa) == isa;
  }

  bool regex_match(std::string str, std::string regex)
  {
#if (defined(__INTEL_COMPILER) && (__INTEL_COMPILER < 1600)) // works around __ZTVNSt3__123__match_any_but_newlineIcEE link error
    return str == regex; 
#elif defined(__GNUC__) && !defined(__clang__) && (__GNUC__ <= 4) && (__GNUC_MINOR__ < 8) // workaround for older gcc version
    return str == regex; 
#else
    std::smatch match; std::regex regexpr(regex);
    return std::regex_match(str, match, regexpr);
#endif
  }

  void AssertNoError(RTCDevice device) 
  {
    RTCError error = rtcGetDeviceError(device);
    if (error != RTC_ERROR_NONE) 
      throw std::runtime_error("Error occured: "+string_of(error));
  }

  void AssertAnyError(RTCDevice device)
  {
    RTCError error = rtcGetDeviceError(device);
    if (error == RTC_ERROR_NONE) 
      throw std::runtime_error("Any error expected");
  }

  void AssertError(RTCDevice device, RTCError expectedError)
  {
    RTCError error = rtcGetDeviceError(device);
    if (error != expectedError) 
      throw std::runtime_error("Error "+string_of(expectedError)+" expected");
  }

  typedef SceneGraph::TriangleMeshNode::Triangle Triangle;

  void addRandomSubdivFeatures(RandomSampler& sampler, Ref<SceneGraph::SubdivMeshNode> mesh, size_t numEdgeCreases, size_t numVertexCreases, size_t numHoles)
  {
    std::vector<unsigned> offsets;
    std::vector<unsigned>& faces = mesh->verticesPerFace;
    std::vector<unsigned>& indices = mesh->position_indices;
    for (size_t i=0, j=0; i<mesh->verticesPerFace.size(); i++) {
      offsets.push_back(unsigned(j)); j+=mesh->verticesPerFace[i];
    } 
    
    for (size_t i=0; i<numEdgeCreases; i++) 
    {
      if (faces.size()) {
        int f = RandomSampler_getInt(sampler) % faces.size();
        int n = faces[f];
        int e = RandomSampler_getInt(sampler) % n;
        mesh->edge_creases.push_back(Vec2i(indices[offsets[f]+(e+0)%n],indices[offsets[f]+(e+1)%n]));
      } else {
        mesh->edge_creases.push_back(Vec2i(0,0));
      }
      mesh->edge_crease_weights.push_back(10.0f*RandomSampler_getFloat(sampler));
    }
    
    for (size_t i=0; i<numVertexCreases; i++) 
    {
      if (faces.size()) {
        size_t f = RandomSampler_getInt(sampler) % faces.size();
        size_t e = RandomSampler_getInt(sampler) % faces[f];
        mesh->vertex_creases.push_back(indices[offsets[f] + e]);
        mesh->vertex_crease_weights.push_back(10.0f*RandomSampler_getFloat(sampler));
      }
    }
    
    for (size_t i=0; i<numHoles; i++) {
      mesh->holes.push_back(RandomSampler_getInt(sampler) % faces.size());
    }
  }    

  struct Sphere
  {
    ALIGNED_CLASS_(16);
  public:
    Sphere () : pos(zero), r(zero) {}
    Sphere (const Vec3fa& pos, float r) : pos(pos), r(r) {}
    __forceinline BBox3fa bounds() const { return BBox3fa(pos-Vec3fa(r),pos+Vec3fa(r)); }
  public:
    Vec3fa pos;
    float r;
  };

  void BoundsFunc(const struct RTCBoundsFunctionArguments* const args)
  {
    Sphere* sphere = (Sphere*) args->geometryUserPtr;
    BBox3fa* bounds_o = (BBox3fa*)args->bounds_o;
    bounds_o->lower.x = sphere->pos.x-sphere->r;
    bounds_o->lower.y = sphere->pos.y-sphere->r;
    bounds_o->lower.z = sphere->pos.z-sphere->r;
    bounds_o->upper.x = sphere->pos.x+sphere->r;
    bounds_o->upper.y = sphere->pos.y+sphere->r;
    bounds_o->upper.z = sphere->pos.z+sphere->r;
  }

  void IntersectFuncN(const struct RTCIntersectFunctionNArguments* const args) {
  }

  void OccludedFuncN(const struct RTCOccludedFunctionNArguments* const args) {
  }

  struct VerifyScene : public RefCount
  {
    VerifyScene (const RTCDeviceRef& device, SceneFlags sflags)
      : device(device), scene(rtcNewScene(device)), flags(sflags)
    {
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);     
    }

    operator RTCScene() const {
      return scene;
    }
    
    unsigned addGeometry(const RTCBuildQuality quality, const Ref<SceneGraph::Node>& node)
    {
      nodes.push_back(node);
      
      if (Ref<SceneGraph::TriangleMeshNode> mesh = node.dynamicCast<SceneGraph::TriangleMeshNode>()) 
      {
        RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom,(unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,mesh->triangles.data(),0,sizeof(SceneGraph::TriangleMeshNode::Triangle), mesh->triangles.size());
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++)
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT3,mesh->positions[t].data(),0,sizeof(SceneGraph::TriangleMeshNode::Vertex), mesh->positions[t].size());
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      }
      else if (Ref<SceneGraph::QuadMeshNode> mesh = node.dynamicCast<SceneGraph::QuadMeshNode>())
      {
        RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_QUAD);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom, (unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT4,mesh->quads.data(),0,sizeof(SceneGraph::QuadMeshNode::Quad), mesh->quads.size());
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++)
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT3,mesh->positions[t].data(),0,sizeof(SceneGraph::QuadMeshNode::Vertex), mesh->positions[t].size());
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      }
      else if (Ref<SceneGraph::GridMeshNode> mesh = node.dynamicCast<SceneGraph::GridMeshNode>())
      {
        RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_GRID);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom, (unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_GRID,0,RTC_FORMAT_GRID,mesh->grids.data(),0,sizeof(SceneGraph::GridMeshNode::Grid), mesh->grids.size());
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++)
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT3,mesh->positions[t].data(),0,sizeof(SceneGraph::GridMeshNode::Vertex), mesh->positions[t].size());
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      } 
      else if (Ref<SceneGraph::SubdivMeshNode> mesh = node.dynamicCast<SceneGraph::SubdivMeshNode>())
      {
        RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_SUBDIVISION);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom, (unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_FACE, 0,RTC_FORMAT_UINT,mesh->verticesPerFace.data(),0,sizeof(int), mesh->verticesPerFace.size());
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT,mesh->position_indices.data(),0,sizeof(int), mesh->position_indices.size());
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++)
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT3,mesh->positions[t].data(),0,sizeof(SceneGraph::SubdivMeshNode::Vertex), mesh->positions[t].size());
        if (mesh->edge_creases.size()) rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_EDGE_CREASE_INDEX,0,RTC_FORMAT_UINT2,mesh->edge_creases.data(),0,2*sizeof(int), mesh->edge_creases.size());
        if (mesh->edge_crease_weights.size()) rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_EDGE_CREASE_WEIGHT,0,RTC_FORMAT_FLOAT,mesh->edge_crease_weights.data(),0,sizeof(float),mesh->edge_crease_weights.size());
        if (mesh->vertex_creases.size()) rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX_CREASE_INDEX,0,RTC_FORMAT_UINT,mesh->vertex_creases.data(),0,sizeof(int), mesh->vertex_creases.size());
        if (mesh->vertex_crease_weights.size()) rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX_CREASE_WEIGHT,0,RTC_FORMAT_FLOAT,mesh->vertex_crease_weights.data(),0,sizeof(float), mesh->vertex_crease_weights.size());
        if (mesh->holes.size()) rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_HOLE,0,RTC_FORMAT_UINT,mesh->holes.data(),0,sizeof(int),mesh->holes.size());
        rtcSetGeometryTessellationRate(geom,mesh->tessellationRate);
        rtcSetGeometrySubdivisionMode(geom,0,mesh->position_subdiv_mode);
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      }
      else if (Ref<SceneGraph::HairSetNode> mesh = node.dynamicCast<SceneGraph::HairSetNode>())
      {
        RTCGeometry geom = rtcNewGeometry (device, mesh->type);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom, (unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++)
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT4,mesh->positions[t].data(),0,sizeof(SceneGraph::HairSetNode::Vertex), mesh->positions[t].size());
        rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT,mesh->hairs.data(),0,sizeof(SceneGraph::HairSetNode::Hair), mesh->hairs.size());
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      } 
      else if (Ref<SceneGraph::PointSetNode> mesh = node.dynamicCast<SceneGraph::PointSetNode>())
      {
        RTCGeometry geom = rtcNewGeometry (device, mesh->type);
        AssertNoError(device);
        rtcSetGeometryTimeStepCount(geom, (unsigned int)mesh->numTimeSteps());
        rtcSetGeometryBuildQuality(geom,quality);
        AssertNoError(device);
        for (unsigned int t=0; t<mesh->numTimeSteps(); t++) {
          rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,t,RTC_FORMAT_FLOAT4,mesh->positions[t].data(),0,sizeof(SceneGraph::PointSetNode::Vertex), mesh->positions[t].size());
          if (mesh->normals.size())
            rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_NORMAL,t,RTC_FORMAT_FLOAT3,mesh->normals[t].data(),0,sizeof(SceneGraph::PointSetNode::Vertex), mesh->normals[t].size());
        }
        AssertNoError(device);
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        return geomID;
      }
      else if (Ref<SceneGraph::TransformNode> mesh = node.dynamicCast<SceneGraph::TransformNode>())
      {
        VerifyScene exemplar(device, flags);
        exemplar.addGeometry(quality, mesh->child);
        rtcCommitScene(exemplar);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(geom, exemplar);
        rtcSetGeometryTimeStepCount(geom, (unsigned) mesh->spaces.size());
        for (size_t i = 0; i < mesh->spaces.size(); ++i)
        {
          rtcSetGeometryTransform(geom,
                                  (unsigned)i,
                                  RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                                  reinterpret_cast<float*>(&mesh->spaces[i]));
        }
        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        return geomID;
      }
      else {
        THROW_RUNTIME_ERROR("unknown node type");
      }
      return 0;
    }

    std::pair<unsigned,Ref<SceneGraph::Node>> addGeometry2(const RTCBuildQuality quality, const Ref<SceneGraph::Node>& node) {
      return std::make_pair(addGeometry(quality,node),node);
    }
    
    std::pair<unsigned,Ref<SceneGraph::Node>> addPlane (RandomSampler& sampler, const RTCBuildQuality quality, size_t num, const Vec3fa& p0, const Vec3fa& dx, const Vec3fa& dy) {
      return addGeometry2(quality,SceneGraph::createTrianglePlane(p0,dx,dy,num,num));
    }
    
    std::pair<unsigned,Ref<SceneGraph::Node>> addSubdivPlane (RandomSampler& sampler, RTCBuildQuality quality, size_t num, const Vec3fa& p0, const Vec3fa& dx, const Vec3fa& dy) {
      return addGeometry2(quality,SceneGraph::createSubdivPlane(p0,dx,dy,num,num,2.0f));
    }
    
    std::pair<unsigned,Ref<SceneGraph::Node>> addSphere (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& pos, const float r, size_t numPhi, size_t maxTriangles = -1, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createTriangleSphere(pos,r,numPhi);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      if (maxTriangles != size_t(-1)) SceneGraph::resize_randomly(sampler,node,maxTriangles);
      return addGeometry2(quality,node);
    }

    std::pair<unsigned,Ref<SceneGraph::Node>> addQuadSphere (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& pos, const float r, size_t numPhi, size_t maxQuads = -1, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createQuadSphere(pos,r,numPhi);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      if (maxQuads != size_t(-1)) SceneGraph::resize_randomly(sampler,node,maxQuads);
      return addGeometry2(quality,node);
    }

    std::pair<unsigned,Ref<SceneGraph::Node>> addGridSphere (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& pos, const float r, size_t N, size_t maxGrids = -1, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createGridSphere(pos,r,N);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      return addGeometry2(quality,node);
    }

    std::pair<unsigned,Ref<SceneGraph::Node>> addSubdivSphere (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& pos, const float r, size_t numPhi, float level, size_t maxFaces = -1, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createSubdivSphere(pos,r,numPhi,level);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      if (maxFaces != size_t(-1)) SceneGraph::resize_randomly(sampler,node,maxFaces);
      addRandomSubdivFeatures(sampler,node.dynamicCast<SceneGraph::SubdivMeshNode>(),10,10,0);
      return addGeometry2(quality,node);
    }
    
    std::pair<unsigned,Ref<SceneGraph::Node>> addSphereHair (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& center, const float radius, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createSphereShapedHair(center,radius);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      return addGeometry2(quality,node);
    }
    
    std::pair<unsigned,Ref<SceneGraph::Node>> addHair (RandomSampler& sampler, RTCBuildQuality quality, const Vec3fa& pos, const float scale, const float r, size_t numHairs = 1, const avector<Vec3fa>& motion_vector = avector<Vec3fa>())
    {
      Ref<SceneGraph::Node> node = SceneGraph::createHairyPlane(RandomSampler_getInt(sampler),pos,Vec3fa(1,0,0),Vec3fa(0,0,1),scale,r,numHairs,SceneGraph::FLAT_CURVE);
      if (motion_vector.size()) SceneGraph::set_motion_vector(node,motion_vector);
      return addGeometry2(quality,node);
    }

    std::pair<unsigned,Ref<SceneGraph::Node>> addUserGeometryEmpty (RandomSampler& sampler, RTCBuildQuality quality, Sphere* sphere)
    {
      RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_USER);
      rtcSetGeometryUserPrimitiveCount(geom,1);
      rtcSetGeometryBuildQuality(geom,quality);
      AssertNoError(device);
      rtcSetGeometryBoundsFunction(geom,BoundsFunc,nullptr);
      rtcSetGeometryUserData(geom,sphere);
      rtcSetGeometryIntersectFunction(geom,IntersectFuncN);
      rtcSetGeometryOccludedFunction(geom,OccludedFuncN);
      rtcCommitGeometry(geom);
      unsigned int geomID = rtcAttachGeometry(scene,geom);
      rtcReleaseGeometry(geom);
      return std::make_pair(geomID,Ref<SceneGraph::Node>(nullptr));
    }

    void resizeRandomly (std::pair<unsigned,Ref<SceneGraph::Node>> geom, RandomSampler& sampler)
    {
      if (Ref<SceneGraph::TriangleMeshNode> mesh = geom.second.dynamicCast<SceneGraph::TriangleMeshNode>())
      {
        rtcSetSharedGeometryBuffer(rtcGetGeometry(scene,geom.first),RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,mesh->triangles.data(),0,sizeof(SceneGraph::TriangleMeshNode::Triangle), RandomSampler_getInt(sampler) % (mesh->triangles.size()+1));
      }
      else if (Ref<SceneGraph::QuadMeshNode> mesh = geom.second.dynamicCast<SceneGraph::QuadMeshNode>())
      {
        rtcSetSharedGeometryBuffer(rtcGetGeometry(scene,geom.first),RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT4,mesh->quads.data(),0,sizeof(SceneGraph::QuadMeshNode::Quad), RandomSampler_getInt(sampler) % (mesh->quads.size()+1));
      } 
      else if (Ref<SceneGraph::SubdivMeshNode> mesh = geom.second.dynamicCast<SceneGraph::SubdivMeshNode>())
      {
        rtcSetSharedGeometryBuffer(rtcGetGeometry(scene,geom.first),RTC_BUFFER_TYPE_FACE,0,RTC_FORMAT_UINT,mesh->verticesPerFace.data(), 0,sizeof(int), RandomSampler_getInt(sampler) % (mesh->verticesPerFace.size()+1));
      }
      else if (Ref<SceneGraph::HairSetNode> mesh = geom.second.dynamicCast<SceneGraph::HairSetNode>())
      {
        rtcSetSharedGeometryBuffer(rtcGetGeometry(scene,geom.first),RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT,mesh->hairs.data(),0,sizeof(SceneGraph::HairSetNode::Hair), RandomSampler_getInt(sampler) % (mesh->hairs.size()+1));
      } 
    }

  public:
    const RTCDeviceRef& device;
    RTCSceneRef scene;
    SceneFlags flags;
    std::vector<Ref<SceneGraph::Node>> nodes;
  };

  VerifyApplication::TestReturnValue VerifyApplication::Test::execute(VerifyApplication* state, bool silent)
  {
    if (!isEnabled())
      return SKIPPED;
    
    if (!silent) 
      std::cout << std::setw(TEXT_ALIGN) << name << " ..." << std::flush;
      
    TestReturnValue v = SKIPPED;
    try {
      v = run(state,silent);
    } catch (...) {
      v = FAILED;
    }
    TestReturnValue ev = ty != TEST_SHOULD_FAIL ? PASSED : FAILED;
    bool passed = v == ev || v == SKIPPED;

    if (silent) 
    {
      Lock<MutexSys> lock(state->mutex);
      if (v != SKIPPED) {
        if      (passed       ) std::cout << state->green ("+") << std::flush;
        else if (ignoreFailure) std::cout << state->yellow("!") << std::flush;
        else                    std::cout << state->red   ("-") << std::flush;
      }
    } 
    else
    {
      if      (v == SKIPPED ) std::cout << state->green (" [SKIPPED]") << std::endl << std::flush;
      else if (passed       ) std::cout << state->green (" [PASSED]" ) << std::endl << std::flush;
      else if (ignoreFailure) std::cout << state->yellow(" [FAILED] (ignored)") << std::endl << std::flush;
      else                    std::cout << state->red   (" [FAILED]" ) << std::endl << std::flush;
    }

    /* update passed/failed counters */
    state->numPassedTests += passed;
    if (ignoreFailure) state->numFailedAndIgnoredTests += !passed;
    else               state->numFailedTests           += !passed;
    
    /* do ignore failures for some specific tests */
    if (ignoreFailure) 
      passed = true;
    //assert(passed);
    return passed ? PASSED : FAILED;
  }

  double VerifyApplication::Benchmark::readDatabase(VerifyApplication* state)
  {
    std::fstream db;
    FileName base = state->database+FileName(name);
    db.open(base.addExt(".txt"), std::fstream::in);
    double start_value = higher_is_better ? double(neg_inf) : double(pos_inf);
    if (!db.is_open()) return start_value;

    double bestAvg = start_value;
    while (true) 
    {
      std::string line; std::getline(db,line);
      if (db.eof()) break;
      if (line == "") { bestAvg = start_value; continue; }
      std::stringstream linestream(line); 
      std::string hash; linestream >> hash;
      double avg; linestream >> avg;
      if (higher_is_better) {
        if (avg > bestAvg) bestAvg = avg;
      } else {
        if (avg < bestAvg) bestAvg = avg;
      }
    }
    db.close();
    return bestAvg;
  }

  void VerifyApplication::Benchmark::updateDatabase(VerifyApplication* state, Statistics stat, double bestAvg)
  {
    /* load git hash from file */
    std::fstream hashFile;
    std::string hash = "unknown";
    hashFile.open(FileName::executableFolder()+FileName("hash"), std::fstream::in);
    if (hashFile.is_open()) {
      hashFile >> hash;
      hashFile.close();
    }

    /* update database */
    std::fstream db;
    FileName base = state->database+FileName(name);
    db.open(base.addExt(".txt"), std::fstream::out | std::fstream::app);
    db << hash << " " << stat.getAvg() << " " << bestAvg << std::endl;
    db.close();
  }

  void VerifyApplication::Benchmark::plotDatabase(VerifyApplication* state)
  {
    std::fstream plot;
    FileName base = state->database+FileName(name);
    plot.open(base.addExt(".plot"), std::fstream::out | std::fstream::trunc);
    plot << "set terminal png size 2048,600 enhanced" << std::endl;
    plot << "set output \"" << base.addExt(".png") << "\"" << std::endl;
    plot << "set key inside right top vertical Right noreverse enhanced autotitles box linetype -1 linewidth 1.000" << std::endl;
    plot << "set samples 50, 50" << std::endl;
    //plot << "set title \"" << name << "\"" << std::endl; 
    //plot << "set xlabel \"" << name << "\""<< std::endl;
    plot << "set xtics axis rotate by 90" << std::endl;
    plot << "set ylabel \"" << unit << "\"" << std::endl;
    plot << "set yrange [0:]" << std::endl;
    plot << "plot \"" << FileName(name).addExt(".txt") << "\" using :2:xtic(1) title \"" << name << "\" with lines, \\" << std::endl; 
    plot << "     \"" << FileName(name).addExt(".txt") << "\" using :3         title \"best\" with lines" << std::endl;
    plot << std::endl;
    plot.close();
  }

  Statistics VerifyApplication::Benchmark::benchmark_loop(VerifyApplication* state)
  {
    //sleepSeconds(0.1);
    const size_t skipBenchmarkFrames = 1;
    const size_t numBenchmarkFrames = 8;
    FilteredStatistics stat(0.5f,0.0f);
    size_t numTotalFrames = skipBenchmarkFrames + numBenchmarkFrames;
    for (size_t i=0; i<skipBenchmarkFrames; i++) {
      benchmark(state);
    }  
    for (size_t i=skipBenchmarkFrames; i<numTotalFrames; i++) {
      stat.add(benchmark(state));
    }
    return stat.getStatistics();
  }

  VerifyApplication::TestReturnValue VerifyApplication::Benchmark::execute(VerifyApplication* state, bool silent) try
  {
    if (!isEnabled())
      return SKIPPED;

    if (!setup(state)) {
      cleanup(state);
      return SKIPPED;
    }
    
    /* print benchmark name */
    std::cout << std::setw(TEXT_ALIGN) << name << ": " << std::flush;
   
    /* read current best from database */
    double avgdb = 0.0f;
    if (state->database != "")
      avgdb = readDatabase(state);

    /* execute benchmark */
    Statistics curStat;
    Statistics bestStat;
    bool passed = false;
    
    /* retry if benchmark failed */
    //static size_t numRetries = 0;
    size_t i=0;
    for (; i<max_attempts && !passed; i++)
    {
      if (i != 0) {
        cleanup(state);
        //if (numRetries++ > 1000) break;
        //std::cout << state->yellow(" [RETRY]" ) << " (" << 100.0f*(curStat.getAvg()-avgdb)/avgdb << "%)" << std::flush;
        setup(state);
      }

      try {
        curStat = benchmark_loop(state);
        if (i == 0) bestStat = curStat;
        if (higher_is_better) {
          if (curStat.getAvg() > bestStat.getAvg()) bestStat = curStat;
        } else {
          if (curStat.getAvg() < bestStat.getAvg()) bestStat = curStat;
        }
      } catch (TestReturnValue v) {
        return v;
      }

      /* check against database to see if test passed */
      if (state->database != "") {
        if (higher_is_better)
          passed = !(curStat.getAvg()-avgdb < -state->benchmark_tolerance*avgdb); // !(a < b) on purpose for nan case
        else
          passed = !(curStat.getAvg()-avgdb > +state->benchmark_tolerance*avgdb); // !(a > b) on purpose for nan case
      }
      else
        passed = true;
    }

    if (state->database == "" || avgdb == double(neg_inf))
      avgdb = bestStat.getAvg();

    /* update database */
    if (state->database != "" && state->update_database)
      updateDatabase(state,bestStat,avgdb);
      
    /* print test result */
    double rate0 = 0; if (bestStat.getAvg()) rate0 = 100.0f*bestStat.getAvgSigma()/bestStat.getAvg();
    double rate1 = 0; if (avgdb            ) rate1 = 100.0f*(bestStat.getAvg()-avgdb)/avgdb;
    
    std::cout << std::setw(8) << std::setprecision(3) << std::fixed << bestStat.getAvg() << " " << unit << " (+/-" << rate0 << "%)";
    if (passed) std::cout << state->green(" [PASSED]" ) << " (" << rate1 << "%) (" << i << " attempts)" << std::endl << std::flush;
    else        std::cout << state->red  (" [FAILED]" ) << " (" << rate1 << "%) (" << i << " attempts)" << std::endl << std::flush;
    if (state->database != "")
      plotDatabase(state);

    /* print dart measurement */
    //if (state->cdash) 
    //{
      /* send plot only when test failed */
      //if (!passed)
      //{
      //  FileName base = state->database+FileName(name);
      //  std::string command = std::string("cd ")+state->database.str()+std::string(" && gnuplot ") + FileName(name).addExt(".plot").str();
      //  if (system(command.c_str()) == 0)
      //    std::cout << "<DartMeasurementFile name=\"" << name << "\" type=\"image/png\">" << base.addExt(".png") << "</DartMeasurementFile>" << std::endl;
      //}
    //}   

    sleepSeconds(0.1);
    cleanup(state);

    /* update passed/failed counters */
    state->numPassedTests += passed;
    state->numFailedTests += !passed;
    return passed ? PASSED : FAILED;
  }
  catch (const std::exception& e)
  {
    std::cout << std::setw(TEXT_ALIGN) << name << ": " << std::flush;
    std::cout << state->red(" [FAILED] ") << "(" << e.what() << ")" << std::endl << std::flush;
    state->numFailedTests++;
    return VerifyApplication::FAILED;
  }
  catch (...)
  {
    std::cout << std::setw(TEXT_ALIGN) << name << ": " << std::flush;
    std::cout << state->red(" [FAILED] ") << " (unknown error)" << std::endl << std::flush;
    state->numFailedTests++;
    return VerifyApplication::FAILED;
  }

  VerifyApplication::TestReturnValue VerifyApplication::TestGroup::execute(VerifyApplication* state, bool silent_in)
  {
    if (!isEnabled())
      return SKIPPED;

    bool leaftest = state->flatten && !silent_in && silent;
    bool nextsilent = silent_in || (state->flatten && silent);
    if (leaftest) 
      std::cout << std::setw(TEXT_ALIGN) << name << " ..." << std::flush;
    
    std::atomic<int> passed(true);

#if defined(__WIN32__) && !defined(__64BIT__)
	/* deactivating parallel test execution on win32 platforms due to out-of-memory exceptions */
	parallel = false;
#endif

    if (state->parallel && parallel && leaftest) 
    {
      parallel_for(tests.size(),[&] (size_t i) {
          passed.fetch_and(tests[i]->execute(state,nextsilent) != FAILED);
        });
    } 
    else {
      for (auto& test : tests)
        passed.fetch_and(test->execute(state,nextsilent) != FAILED);
    }

    if (leaftest) {
      if (passed) std::cout << state->green(" [PASSED]") << std::endl << std::flush;
      else        std::cout << state->red  (" [FAILED]") << std::endl << std::flush;
    }

    return passed ? PASSED : FAILED;
  }

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  struct DeviceCreationTest : public VerifyApplication::Test
  {
    DeviceCreationTest (std::string name)
      : VerifyApplication::Test(name,0,VerifyApplication::TEST_SHOULD_PASS) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      RTCDevice device = rtcNewDevice("verbose=1");
      errorHandler(nullptr,rtcGetDeviceError(device));
      rtcReleaseDevice(device);
      return VerifyApplication::PASSED;
    }
  };

  struct EmbreeInternalTest : public VerifyApplication::Test
  {
    EmbreeInternalTest (std::string name, size_t testID)
      : VerifyApplication::Test(name,0,VerifyApplication::TEST_SHOULD_PASS), testID(testID) {}
  
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      RTCDeviceRef device = rtcNewDevice(state->rtcore.c_str());
      AssertNoError(device);
      return (VerifyApplication::TestReturnValue)rtcGetDeviceProperty(device,(RTCDeviceProperty)(3000000+testID));
    }

    size_t testID;
  };

  struct os_shrink_test : public VerifyApplication::Test
  {
    struct Allocation 
    {
      Allocation (size_t bytes) 
        : ptr(os_malloc(bytes,hugepages)), bytes(bytes) {}

      ~Allocation() {
        free();
      }

      void free() {
        if (!ptr) return;
        os_free(ptr,bytes,hugepages);
        ptr = nullptr;
      }

      size_t shrink() {
        if (!ptr) return 0;
        bytes = os_shrink(ptr,bytes/2,bytes,hugepages);
        return bytes;
      }

    private:
      void* ptr;
      size_t bytes;
      bool hugepages;
    };

    os_shrink_test ()
      : VerifyApplication::Test("os_shrink_test",ISA,VerifyApplication::TEST_SHOULD_PASS,false) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::vector<std::unique_ptr<Allocation>> allocations;
      allocations.reserve(1024*1024);
      for (size_t i=0; i<1024*1024; i++) 
      {
        std::unique_ptr<Allocation> alloc(new Allocation(2*4096));
        if (allocations.size() > 1) allocations.back()->shrink();
        allocations.push_back(std::move(alloc)); 
        
      }
      allocations.clear();
      return VerifyApplication::PASSED;
    }
  };

  struct MultipleDevicesTest : public VerifyApplication::Test
  {
    MultipleDevicesTest (std::string name, int isa)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDevice device1 = rtcNewDevice(cfg.c_str());
      AssertNoError(device1);
      RTCDevice device2 = rtcNewDevice(cfg.c_str());
      AssertNoError(device2);
      RTCDevice device3 = rtcNewDevice(cfg.c_str());
      AssertNoError(device3);
      rtcReleaseDevice(device1);
      rtcReleaseDevice(device3);
      rtcReleaseDevice(device2);
      return VerifyApplication::PASSED;
    }
  };

  struct GetBoundsTest : public VerifyApplication::Test
  {
    GeometryType gtype;

    GetBoundsTest (std::string name, int isa, GeometryType gtype)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), gtype(gtype) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM));
      AssertNoError(device);

      Ref<SceneGraph::Node> node = nullptr;
      switch (gtype) {
      case TRIANGLE_MESH   : node = SceneGraph::createTriangleSphere(zero,1.0f,50); break;
      case TRIANGLE_MESH_MB: node = SceneGraph::createTriangleSphere(zero,1.0f,5)->set_motion_vector(Vec3fa(1.0f)); break;
      case QUAD_MESH       : node = SceneGraph::createQuadSphere(zero,1.0f,50); break;
      case QUAD_MESH_MB    : node = SceneGraph::createQuadSphere(zero,1.0f,50)->set_motion_vector(Vec3fa(1.0f)); break;
      case GRID_MESH       : node = SceneGraph::createGridSphere(zero,1.0f,50); break;
      case GRID_MESH_MB    : node = SceneGraph::createGridSphere(zero,1.0f,50)->set_motion_vector(Vec3fa(1.0f)); break;
        //case SUBDIV_MESH:
        //case SUBDIV_MESH_MB:
      default: return VerifyApplication::SKIPPED;
      }

      BBox3fa bounds0 = node->bounds();
      scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,node);
      AssertNoError(device);
      rtcCommitScene (scene);
      AssertNoError(device);
      BBox3fa bounds1;
      rtcGetSceneBounds(scene,(RTCBounds*)&bounds1);
      AssertNoError(device);
      return (VerifyApplication::TestReturnValue)(bounds0 == bounds1);
    }
  };
  
  struct GetLinearBoundsTest : public VerifyApplication::Test
  {
    GeometryType gtype;

    GetLinearBoundsTest (std::string name, int isa, GeometryType gtype)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), gtype(gtype) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM));
      AssertNoError(device);

      Ref<SceneGraph::Node> node = nullptr;
      switch (gtype) {
      case TRIANGLE_MESH   : node = SceneGraph::createTriangleSphere(zero,1.0f,50); break;
      case TRIANGLE_MESH_MB: node = SceneGraph::createTriangleSphere(zero,1.0f,50)->set_motion_vector(Vec3fa(1.0f)); break;
      case QUAD_MESH       : node = SceneGraph::createQuadSphere(zero,1.0f,50); break;
      case QUAD_MESH_MB    : node = SceneGraph::createQuadSphere(zero,1.0f,50)->set_motion_vector(Vec3fa(1.0f)); break;
      case GRID_MESH       : node = SceneGraph::createGridSphere(zero,1.0f,50); break;
      case GRID_MESH_MB    : node = SceneGraph::createGridSphere(zero,1.0f,50)->set_motion_vector(Vec3fa(1.0f)); break;
        //case SUBDIV_MESH:
        //case SUBDIV_MESH_MB:
      default: return VerifyApplication::SKIPPED;
      }

      LBBox3fa bounds0 = node->lbounds();
      scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,node);
      AssertNoError(device);
      rtcCommitScene (scene);
      AssertNoError(device);
      LBBox3fa bounds1;
      rtcGetSceneLinearBounds(scene,(RTCLinearBounds*)&bounds1);
      AssertNoError(device);
      return (VerifyApplication::TestReturnValue)(bounds0 == bounds1);
    }
  };

  struct GetUserDataTest : public VerifyApplication::Test
  {
    GetUserDataTest (std::string name, int isa)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      RTCGeometry geom0 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryUserData(geom0,(void*)1);
      RTCGeometry geom1 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_QUAD);
      rtcSetGeometryUserData(geom1,(void*)2);
      RTCGeometry geom2 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_GRID);
      rtcSetGeometryUserData(geom2,(void*)3);
      RTCGeometry geom3 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SUBDIVISION);
      rtcSetGeometryUserData(geom3,(void*)4);
      RTCGeometry geom4 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_FLAT_LINEAR_CURVE);
      rtcSetGeometryUserData(geom4,(void*)5);
      RTCGeometry geom5 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE);
      rtcSetGeometryUserData(geom5,(void*)6);
      RTCGeometry geom6 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
      rtcSetGeometryUserData(geom6,(void*)7);
      AssertNoError(device);
      
      if ((size_t)rtcGetGeometryUserData(geom0 ) !=  1) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom1 ) !=  2) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom2 ) !=  3) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom3 ) !=  4) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom4 ) !=  5) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom5 ) !=  6) return VerifyApplication::FAILED;
      if ((size_t)rtcGetGeometryUserData(geom6 ) !=  7) return VerifyApplication::FAILED;
      AssertNoError(device);

      rtcReleaseGeometry(geom0);
      rtcReleaseGeometry(geom1);
      rtcReleaseGeometry(geom2);
      rtcReleaseGeometry(geom3);
      rtcReleaseGeometry(geom4);
      rtcReleaseGeometry(geom5);
      rtcReleaseGeometry(geom6);

      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct BufferStrideTest : public VerifyApplication::Test
  {
    GeometryType gtype;

    BufferStrideTest (std::string name, int isa, GeometryType gtype)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), gtype(gtype) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      RTCGeometry geom = nullptr;
      switch (gtype) {
      case TRIANGLE_MESH    : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE); break;
      case TRIANGLE_MESH_MB : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE); break;
      case QUAD_MESH        : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_QUAD); break;
      case QUAD_MESH_MB     : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_QUAD); break;
      case SUBDIV_MESH      : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SUBDIVISION); break;
      case SUBDIV_MESH_MB   : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SUBDIVISION); break;
      case BEZIER_GEOMETRY    : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_FLAT_BEZIER_CURVE); break;
      case BEZIER_GEOMETRY_MB : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_FLAT_BEZIER_CURVE); break;
      case BSPLINE_GEOMETRY   : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE); break;
      case BSPLINE_GEOMETRY_MB: geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_BSPLINE_CURVE); break;
      case CATMULL_GEOMETRY   : geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE); break;
      case CATMULL_GEOMETRY_MB: geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_CATMULL_ROM_CURVE); break;
      default               : return VerifyApplication::PASSED;
      }
      AssertNoError(device);

      RTCFormat indexFormat;
      RTCFormat vertexFormat;
      switch (gtype) {
      case TRIANGLE_MESH:
      case TRIANGLE_MESH_MB:
        indexFormat  = RTC_FORMAT_UINT3;
        vertexFormat = RTC_FORMAT_FLOAT3;
        break;

      case QUAD_MESH:
      case QUAD_MESH_MB:
        indexFormat  = RTC_FORMAT_UINT4;
        vertexFormat = RTC_FORMAT_FLOAT3;
        break;

      case SUBDIV_MESH:
      case SUBDIV_MESH_MB:
        indexFormat  = RTC_FORMAT_UINT;
        vertexFormat = RTC_FORMAT_FLOAT3;
        break;

      case BEZIER_GEOMETRY:
      case BEZIER_GEOMETRY_MB:
      case BSPLINE_GEOMETRY:
      case BSPLINE_GEOMETRY_MB:
      case CATMULL_GEOMETRY:
      case CATMULL_GEOMETRY_MB:
        indexFormat  = RTC_FORMAT_UINT;
        vertexFormat = RTC_FORMAT_FLOAT4;
        break;

      default:
        indexFormat  = RTC_FORMAT_UNDEFINED;
        vertexFormat = RTC_FORMAT_UNDEFINED;
        break;
      }

      avector<char> indexBuffer(8+16*6*sizeof(int));
      avector<char> vertexBuffer(12+16*9*sizeof(float)+4);
      
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,indexFormat,indexBuffer.data(),1,3*sizeof(int),16);
      AssertError(device,RTC_ERROR_INVALID_OPERATION);
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,vertexFormat,vertexBuffer.data(),1,3*sizeof(float),16);
      AssertError(device,RTC_ERROR_INVALID_OPERATION);

      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,indexFormat,indexBuffer.data(),0,3*sizeof(int)+3,16);
      AssertError(device,RTC_ERROR_INVALID_OPERATION);
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,vertexFormat,vertexBuffer.data(),0,3*sizeof(float)+3,16);
      AssertError(device,RTC_ERROR_INVALID_OPERATION);

      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,indexFormat,indexBuffer.data(),0,3*sizeof(int),16);
      AssertNoError(device);
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,vertexFormat,vertexBuffer.data(),0,3*sizeof(float),16);
      AssertNoError(device);

      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,indexFormat,indexBuffer.data(),8,6*sizeof(int),16);
      AssertNoError(device);
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,vertexFormat,vertexBuffer.data(),12,9*sizeof(float),16);
      AssertNoError(device);

      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_INDEX,0,indexFormat,indexBuffer.data(),0,3*sizeof(int),16);
      AssertNoError(device);
      
      rtcSetSharedGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,vertexFormat,vertexBuffer.data(),0,4*sizeof(float),16);
      AssertNoError(device);

      rtcReleaseGeometry(geom);
      AssertNoError(device);
      
      return VerifyApplication::PASSED;
    }
  };

  /////////////////////////////////////////////////////////////////////////////////

  /*
   * Test that types can be instantiated.
   */
  struct TypesTest : public VerifyApplication::Test
  {
    TypesTest(std::string name, int isa)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS)
    {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      return VerifyApplication::PASSED;
    }

    BBox<Vec2<int>> bbv2i;
    BBox<Vec3<int>> bbv3i;
    BBox<Vec4<int>> bbv4i;
    BBox<Vec2<unsigned int>> bbv2u;
    BBox<Vec3<unsigned int>> bbv3u;
    BBox<Vec4<unsigned int>> bbv4u;
    BBox<Vec2<float>> bbv2f;
    BBox<Vec3<float>> bbv3f;
    BBox<Vec4<float>> bbv4f;
    BBox<Vec2<double>> bbv2d;
    BBox<Vec3<double>> bbv3d;
    BBox<Vec4<double>> bbv4d;
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  struct EmptySceneTest : public VerifyApplication::Test
  {
    EmptySceneTest (std::string name, int isa, SceneFlags sflags)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);
      AssertNoError(device);
      rtcCommitScene (scene);
      AssertNoError(device);
      return VerifyApplication::PASSED;
    }

  public:
    SceneFlags sflags;
  };

  void rtcCommitAndAttachAndReleaseGeometry(RTCScene scene, RTCGeometry geometry, RTCBuildQuality quality, unsigned int timeSteps)
  {
    rtcSetGeometryBuildQuality(geometry,quality);
    rtcSetGeometryTimeStepCount(geometry,timeSteps);
    rtcCommitGeometry(geometry);
    rtcAttachGeometry(scene,geometry);
    rtcReleaseGeometry(geometry);
  }
  
  struct EmptyGeometryTest : public VerifyApplication::Test
  {
    SceneFlags sflags;
    RTCBuildQuality quality;

    EmptyGeometryTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);     
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE),quality,2);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_QUAD),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_QUAD),quality,2);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_GRID),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_GRID),quality,2);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_SUBDIVISION),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_SUBDIVISION),quality,2);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE),quality,2);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_USER),quality,1);
      rtcCommitAndAttachAndReleaseGeometry(scene,rtcNewGeometry (device, RTC_GEOMETRY_TYPE_USER),quality,2);
      rtcCommitScene (scene);
      AssertNoError(device);
      return VerifyApplication::PASSED;
    }
  };

  struct ManyBuildTest : public VerifyApplication::Test
  {
    SceneFlags sflags;
    RTCBuildQuality quality; 
    
    ManyBuildTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS,false), sflags(sflags), quality(quality) {}
    
    VerifyApplication::TestReturnValue run (VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      rtcSetDeviceErrorFunction(device,errorHandler,nullptr);
      
      std::vector<Vec3f> p;
      p.reserve(4);
      p.push_back(Vec3f(0.0f, 0.0f, 0.0f));
      p.push_back(Vec3f(1.0f, 0.0f, 0.0f));
      p.push_back(Vec3f(0.0f, 0.5f, 1.0f));
      
      std::vector<uint32_t> indices;
      indices.push_back(0);
      indices.push_back(1);
      indices.push_back(2);
      
      const size_t numTriangles = 1;
      const size_t numVertices = 3;
      
      for (size_t i = 0; i < 1024*1024; ++i)
      {
        //rtcSetDeviceProperty(nullptr,(RTCDeviceProperty) 1000000, i);
        
        RTCScene scene = rtcNewScene(device);
        rtcSetSceneFlags(scene,sflags.sflags);
        rtcSetSceneBuildQuality(scene,sflags.qflags);
                
        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        rtcSetGeometryBuildQuality(geom,quality);
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, p.data(), 0, 3 * sizeof(float), numVertices);
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, indices.data(), 0, 3 * sizeof(uint32_t), numTriangles);
        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene,geom);
        rtcReleaseGeometry(geom);
        
        rtcCommitScene(scene);
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct BuildTest : public VerifyApplication::Test
  {
    SceneFlags sflags;
    RTCBuildQuality quality; 

    BuildTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}
    
    VerifyApplication::TestReturnValue run (VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,sflags);

      const Vec3fa center = zero;
      const float radius = 1.0f;
      const Vec3fa dx(1,0,0);
      const Vec3fa dy(0,1,0);
      scene.addGeometry(quality,SceneGraph::createTriangleSphere(center,radius,50));
      scene.addGeometry(quality,SceneGraph::createTriangleSphere(center,radius,50)->set_motion_vector(random_motion_vector(1.0f)));
      scene.addGeometry(quality,SceneGraph::createQuadSphere(center,radius,50));
      scene.addGeometry(quality,SceneGraph::createQuadSphere(center,radius,50)->set_motion_vector(random_motion_vector(1.0f)));
      scene.addGeometry(quality,SceneGraph::createGridSphere(center,radius,50));
      scene.addGeometry(quality,SceneGraph::createGridSphere(center,radius,50)->set_motion_vector(random_motion_vector(1.0f)));
      scene.addGeometry(quality,SceneGraph::createSubdivSphere(center,radius,8,20));
      scene.addGeometry(quality,SceneGraph::createSubdivSphere(center,radius,8,20)->set_motion_vector(random_motion_vector(1.0f)));
      scene.addGeometry(quality,SceneGraph::createHairyPlane(RandomSampler_getInt(sampler),center,dx,dy,0.1f,0.01f,100,SceneGraph::FLAT_CURVE));
      scene.addGeometry(quality,SceneGraph::createHairyPlane(RandomSampler_getInt(sampler),center,dx,dy,0.1f,0.01f,100,SceneGraph::FLAT_CURVE)->set_motion_vector(random_motion_vector(1.0f)));
      rtcCommitScene (scene);
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct OverlappingGeometryTest : public VerifyApplication::Test
  {
    SceneFlags sflags;
    RTCBuildQuality quality; 
    size_t N;
    
    OverlappingGeometryTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, size_t N)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality), N(N) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,sflags);
      AssertNoError(device);

      const Vec3fa p (0,0,0);
      const Vec3fa dx(1,0,0);
      const Vec3fa dy(0,1,0);
      
      Ref<SceneGraph::TriangleMeshNode> trimesh = SceneGraph::createTrianglePlane(p,dx,dy,1,1).dynamicCast<SceneGraph::TriangleMeshNode>();
      for (size_t i=0; i<N; i++) trimesh->triangles.push_back(trimesh->triangles.back());
      scene.addGeometry(quality,trimesh.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::TriangleMeshNode> trimesh2 = SceneGraph::createTrianglePlane(p,dx,dy,1,1)->set_motion_vector(random_motion_vector(1.0f)).dynamicCast<SceneGraph::TriangleMeshNode>();
      for (size_t i=0; i<N; i++) trimesh2->triangles.push_back(trimesh2->triangles.back());
      scene.addGeometry(quality,trimesh2.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::QuadMeshNode> quadmesh = SceneGraph::createQuadPlane(p,dx,dy,1,1).dynamicCast<SceneGraph::QuadMeshNode>();
      for (size_t i=0; i<N; i++) quadmesh->quads.push_back(quadmesh->quads.back());
      scene.addGeometry(quality,quadmesh.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::QuadMeshNode> quadmesh2 = SceneGraph::createQuadPlane(p,dx,dy,1,1)->set_motion_vector(random_motion_vector(1.0f)).dynamicCast<SceneGraph::QuadMeshNode>();
      for (size_t i=0; i<N; i++) quadmesh2->quads.push_back(quadmesh2->quads.back());
      scene.addGeometry(quality,quadmesh2.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::GridMeshNode> gridmesh = SceneGraph::createGridPlane(p,dx,dy,1,1).dynamicCast<SceneGraph::GridMeshNode>();
      for (size_t i=0; i<N; i++) gridmesh->grids.push_back(gridmesh->grids.back());
      scene.addGeometry(quality,gridmesh.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::GridMeshNode> gridmesh2 = SceneGraph::createGridPlane(p,dx,dy,1,1)->set_motion_vector(random_motion_vector(1.0f)).dynamicCast<SceneGraph::GridMeshNode>();
      for (size_t i=0; i<N; i++) gridmesh2->grids.push_back(gridmesh2->grids.back());
      scene.addGeometry(quality,gridmesh2.dynamicCast<SceneGraph::Node>());
      
      Ref<SceneGraph::SubdivMeshNode> subdivmesh = new SceneGraph::SubdivMeshNode(nullptr,BBox1f(0,1),1);
      for (unsigned i=0; i<unsigned(N); i++) {
        subdivmesh->verticesPerFace.push_back(4);
        subdivmesh->position_indices.push_back(4*i+0);
        subdivmesh->position_indices.push_back(4*i+1);
        subdivmesh->position_indices.push_back(4*i+2);
        subdivmesh->position_indices.push_back(4*i+3);
        subdivmesh->positions[0].push_back(Vec3fa(0,0,0));
        subdivmesh->positions[0].push_back(Vec3fa(0,1,0));
        subdivmesh->positions[0].push_back(Vec3fa(1,1,0));
        subdivmesh->positions[0].push_back(Vec3fa(1,0,0));
      }
      scene.addGeometry(quality,subdivmesh.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::SubdivMeshNode> subdivmesh2 = new SceneGraph::SubdivMeshNode(nullptr,BBox1f(0,1),1);
      for (unsigned i=0; i<unsigned(N); i++) {
        subdivmesh2->verticesPerFace.push_back(4);
        subdivmesh2->position_indices.push_back(4*i+0);
        subdivmesh2->position_indices.push_back(4*i+1);
        subdivmesh2->position_indices.push_back(4*i+2);
        subdivmesh2->position_indices.push_back(4*i+3);
        subdivmesh2->positions[0].push_back(Vec3fa(0,0,0));
        subdivmesh2->positions[0].push_back(Vec3fa(0,1,0));
        subdivmesh2->positions[0].push_back(Vec3fa(1,1,0));
        subdivmesh2->positions[0].push_back(Vec3fa(1,0,0));
      }
      subdivmesh2->set_motion_vector(random_motion_vector(1.0f));
      scene.addGeometry(quality,subdivmesh2.dynamicCast<SceneGraph::Node>());
      
      Ref<SceneGraph::HairSetNode> hairgeom = SceneGraph::createHairyPlane(RandomSampler_getInt(sampler),p,dx,dy,0.2f,0.01f,1,SceneGraph::FLAT_CURVE).dynamicCast<SceneGraph::HairSetNode>();
      for (size_t i=0; i<N; i++) hairgeom->hairs.push_back(hairgeom->hairs.back());
      scene.addGeometry(quality,hairgeom.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::HairSetNode> hairgeom2 = SceneGraph::createHairyPlane(RandomSampler_getInt(sampler),p,dx,dy,0.2f,0.01f,1,SceneGraph::FLAT_CURVE)->set_motion_vector(random_motion_vector(1.0f)).dynamicCast<SceneGraph::HairSetNode>();
      for (size_t i=0; i<N; i++) hairgeom2->hairs.push_back(hairgeom2->hairs.back());
      scene.addGeometry(quality,hairgeom2.dynamicCast<SceneGraph::Node>());

      Ref<SceneGraph::Node> curvegeom = SceneGraph::convert_flat_to_round_curves(hairgeom.dynamicCast<SceneGraph::Node>());
      scene.addGeometry(quality,curvegeom);

      Ref<SceneGraph::Node> curvegeom2 = SceneGraph::convert_flat_to_round_curves(hairgeom2.dynamicCast<SceneGraph::Node>());
      scene.addGeometry(quality,curvegeom2);

      Ref<SceneGraph::Node> linegeom = SceneGraph::convert_bezier_to_lines(hairgeom.dynamicCast<SceneGraph::Node>());
      scene.addGeometry(quality,linegeom);

      Ref<SceneGraph::Node> linegeom2 = SceneGraph::convert_bezier_to_lines(hairgeom2.dynamicCast<SceneGraph::Node>());
      scene.addGeometry(quality,linegeom2);
      
      rtcCommitScene (scene);
      AssertNoError(device);
      return VerifyApplication::PASSED;
    }
  };

  static std::atomic<ssize_t> memory_consumption_bytes_used(0);

  struct MemoryConsumptionTest : public VerifyApplication::Test
  {
    GeometryType gtype;
    SceneFlags sflags;
    RTCBuildQuality quality;
    
    MemoryConsumptionTest (std::string name, int isa, GeometryType gtype, SceneFlags sflags, RTCBuildQuality quality)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), gtype(gtype), sflags(sflags), quality(quality) {}

    static bool memoryMonitor(void* userPtr, const ssize_t bytes, const bool /*post*/)
    {
      memory_consumption_bytes_used += bytes;
      return true;
    }

    double expected_size_helper(VerifyApplication* state, size_t NN)
    {
      bool has_avx = (isa & AVX) == AVX;
      bool has_avx2 = (isa & AVX2) == AVX2;
      
      if (sflags.qflags == RTC_BUILD_QUALITY_LOW)
      {
        switch (gtype) {
        case TRIANGLE_MESH: return has_avx ? 131.0f*NN : 117.0f*NN; // triangle4
        default: return inf;
        }
      }
      else
      {
        switch (gtype)
        {
        case TRIANGLE_MESH: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx ?  70.0f*NN :  63.0f*NN; // triangle4
          case RTC_SCENE_FLAG_ROBUST : return has_avx ?  70.0f*NN :  63.0f*NN; // triangle4v
          case RTC_SCENE_FLAG_COMPACT: return has_avx ?  35.0f*NN :  35.0f*NN; // triangle4i
          default: return inf;
          }
          
          
        case TRIANGLE_MESH_MB: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx2 ? 55.0f*NN : 45.0f*NN; // triangle4imb
          case RTC_SCENE_FLAG_ROBUST : return has_avx2 ? 55.0f*NN : 45.0f*NN; // triangle4imb
          case RTC_SCENE_FLAG_COMPACT: return has_avx2 ? 45.0f*NN : 45.0f*NN; // triangle4imb
          default: return inf;
          }
          
        case QUAD_MESH: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx ? 85.0f*NN : 79.0f*NN; // quad4v
          case RTC_SCENE_FLAG_ROBUST : return has_avx ? 85.0f*NN : 79.0f*NN; // quad4v
          case RTC_SCENE_FLAG_COMPACT: return has_avx ? 41.0f*NN : 41.0f*NN; // quad4i
          default: return inf;
          }
        case QUAD_MESH_MB: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx ? 68.0f*NN : 53.0f*NN; // quad4imb
          case RTC_SCENE_FLAG_ROBUST : return has_avx ? 68.0f*NN : 53.0f*NN; // quad4imb
          case RTC_SCENE_FLAG_COMPACT: return has_avx ? 53.0f*NN : 53.0f*NN; // quad4imb
          default: return inf;
          }
          
        case BEZIER_GEOMETRY: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx2 ?  222.0f*NN : 165.0f*NN; // bezier1v
          case RTC_SCENE_FLAG_ROBUST : return has_avx2 ?  222.0f*NN : 165.0f*NN; // bezier1v
          case RTC_SCENE_FLAG_COMPACT: return has_avx2 ?  105.0f*NN : 105.0f*NN; // bezier1i
          default: return inf;
          }
          
        case BEZIER_GEOMETRY_MB: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx2 ?  386.0f*NN : 190.0f*NN; // bezier1i // FIXME: 386 are very loose bounds
          case RTC_SCENE_FLAG_ROBUST : return has_avx2 ?  386.0f*NN : 190.0f*NN; // bezier1i // FIXME: 386 are very loose bounds 
          case RTC_SCENE_FLAG_COMPACT: return has_avx2 ?  190.0f*NN : 190.0f*NN; // bezier1i
          default: return inf;
          }
          
        case LINE_GEOMETRY: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx ? 32.0f*NN : 26.0f*NN; // line4i
          case RTC_SCENE_FLAG_ROBUST : return has_avx ? 32.0f*NN : 26.0f*NN; // line4i
          case RTC_SCENE_FLAG_COMPACT: return has_avx ? 26.0f*NN : 26.0f*NN; // line4i
          default: return inf;
          }
          
        case LINE_GEOMETRY_MB: switch (sflags.sflags) {
          case RTC_SCENE_FLAG_NONE: return has_avx ? 45.0f*NN : 36.0f*NN; // line4i
          case RTC_SCENE_FLAG_ROBUST : return has_avx ? 45.0f*NN : 36.0f*NN; // line4i
          case RTC_SCENE_FLAG_COMPACT: return has_avx ? 36.0f*NN : 36.0f*NN; // line4i
          default: return inf;
          }
          
        default: return inf;
        }
      }
    }
    
    double expected_size(VerifyApplication* state, size_t NN)
    {
      double bytes_expected = expected_size_helper(state,NN);
      bool use_single_mode = false; //bytes_expected < 100000;
      double mainBlockSize = clamp(bytes_expected/20,1024.0,double(2*1024*1024-64));
      double threadLocalBlockSize = clamp(bytes_expected/20,double(1024),double(PAGE_SIZE));
      double expected = bytes_expected + ceil(bytes_expected/mainBlockSize)*128 + ceil(bytes_expected/threadLocalBlockSize)*128 + mainBlockSize;
      if (use_single_mode == false) expected += mainBlockSize;
      return expected;
    }

    std::pair<ssize_t,ssize_t> run_build(VerifyApplication* state, size_t N, unsigned numThreads)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa) + ",threads="+std::to_string((long long)numThreads);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      memory_consumption_bytes_used = 0;
      rtcSetDeviceMemoryMonitorFunction(device,memoryMonitor,nullptr);
      VerifyScene scene(device,sflags);
      AssertNoError(device);
      
      int numPhi = (size_t) ceilf(sqrtf(N/4.0f));
      ssize_t NN = 0;

      float plen = sqrtf(N/100000.0f);
      Vec3fa planeX = plen*normalize(Vec3fa(1,1,0));
      Vec3fa planeY = plen*normalize(Vec3fa(0,1,1));
      
      Ref<SceneGraph::Node> mesh;
      int i = 0;
      switch (gtype) {
      case TRIANGLE_MESH:    
      case TRIANGLE_MESH_MB: mesh = SceneGraph::createTriangleSphere(zero,float(i+1),numPhi); break;
      case QUAD_MESH:        
      case QUAD_MESH_MB:     mesh = SceneGraph::createQuadSphere(zero,float(i+1),numPhi); break;
      case SUBDIV_MESH:      
      case SUBDIV_MESH_MB:   mesh = SceneGraph::createSubdivSphere(zero,float(i+1),8,float(numPhi)/8.0f); break;
      case BEZIER_GEOMETRY:    
      case BEZIER_GEOMETRY_MB: 
      case BSPLINE_GEOMETRY:
      case BSPLINE_GEOMETRY_MB:
      case CATMULL_GEOMETRY:
      case CATMULL_GEOMETRY_MB: mesh = SceneGraph::createHairyPlane(i,Vec3fa(float(i)),planeX,planeY,0.01f,0.00001f,4*numPhi*numPhi,SceneGraph::FLAT_CURVE); break;
      case LINE_GEOMETRY:    
      case LINE_GEOMETRY_MB: mesh = SceneGraph::createHairyPlane(i,Vec3fa(float(i)),planeX,planeY,0.01f,0.00001f,4*numPhi*numPhi/3,SceneGraph::FLAT_CURVE); break;
      default:               throw std::runtime_error("invalid geometry for benchmark");
      }
      
      switch (gtype) {
      case LINE_GEOMETRY:    
      case LINE_GEOMETRY_MB: mesh = SceneGraph::convert_bezier_to_lines(mesh); break;
      default: break;
      }
      
      switch (gtype) {
      case TRIANGLE_MESH_MB: 
      case QUAD_MESH_MB:     
      case SUBDIV_MESH_MB:   
      case BEZIER_GEOMETRY_MB: 
      case BSPLINE_GEOMETRY_MB:
      case CATMULL_GEOMETRY_MB:
      case LINE_GEOMETRY_MB: mesh = mesh->set_motion_vector(random_motion_vector2(0.0001f)); break;
      default: break;
      }
      NN = mesh->numPrimitives(); 
      scene.addGeometry(quality,mesh);
      rtcCommitScene (scene);
      AssertNoError(device);
      
      return std::make_pair(NN,memory_consumption_bytes_used.load());
    }
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      VerifyApplication::TestReturnValue ret = VerifyApplication::PASSED;

      size_t maxN = 0;
      switch (gtype) {
      case LINE_GEOMETRY:    
      case LINE_GEOMETRY_MB: 
      case BEZIER_GEOMETRY:
      case BEZIER_GEOMETRY_MB:
      case BSPLINE_GEOMETRY:
      case BSPLINE_GEOMETRY_MB:
      case CATMULL_GEOMETRY:
      case CATMULL_GEOMETRY_MB: maxN = 250000; break;
      default: maxN = 1000000; break;
      }
      
      for (size_t N=128; N<maxN; N = (size_t)((float)N * 1.5f)) 
      {
        auto bytes_one_thread  = run_build(state,N,1);
        auto bytes_all_threads = run_build(state,N,0);
        double bytes_expected = expected_size(state,bytes_one_thread.first);
        double expected_to_single = double(bytes_one_thread.second)/double(bytes_expected);
        double single_to_threaded = double(bytes_all_threads.second)/double(bytes_one_thread.second);
     
        const bool failed0 = expected_to_single > 1.0f;
        const bool failed1 = (sflags.qflags == RTC_BUILD_QUALITY_LOW) ? single_to_threaded > 1.35f : single_to_threaded > 1.12f;

        if (failed0 || failed1) {
          std::cout << state->red ("-") << std::flush;
          ret = VerifyApplication::FAILED;
        } else {
          std::cout << state->green ("+") << std::flush;
        }

        if (failed0 || failed1)
        {
          double num_primitives = (double)bytes_one_thread.first;
          std::cout << "N = " << num_primitives << ", n = " << ceilf(sqrtf(N/4.0f)) << ", "
            "expected = " << bytes_expected/num_primitives << " B, " << 
            "1 thread = " << bytes_one_thread.second/num_primitives << " B (" << 100.0f*expected_to_single << " %)" << (failed0 ? state->red(" [FAILED]") : "") << ", " << 
            "all_threads = " << bytes_all_threads.second/num_primitives << " B (" << 100.0f*single_to_threaded << " %)" << (failed1 ? state->red(" [FAILED]") : "") << std::endl;
        }
      }
      return ret;
    }
  };
    
  struct NewDeleteGeometryTest : public VerifyApplication::Test
  {
    SceneFlags sflags;

    NewDeleteGeometryTest (std::string name, int isa, SceneFlags sflags)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,sflags);
      AssertNoError(device);
      unsigned int geom[128];
      for (size_t i=0; i<128; i++) geom[i] = -1;
      Sphere spheres[128];
      memset(spheres,0,sizeof(spheres));
      
      for (size_t i=0; i<size_t(50*state->intensity); i++) 
      {
        for (size_t j=0; j<10; j++) {
          int index = random_int()%128;
          Vec3fa pos = 100.0f*random_Vec3fa();
          if (geom[index] == -1) {
            switch (random_int()%11) {
            case 0: geom[index] = scene.addSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10).first; break;
            case 1: geom[index] = scene.addSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10,-1,random_motion_vector(1.0f)).first; break;
            case 2: geom[index] = scene.addQuadSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10).first; break;
            case 3: geom[index] = scene.addQuadSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10,-1,random_motion_vector(1.0f)).first; break;
            case 4: geom[index] = scene.addGridSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10).first; break;
            case 5: geom[index] = scene.addGridSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,10,-1,random_motion_vector(1.0f)).first; break;
            case 6: geom[index] = scene.addHair(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,1.0f,2.0f,10).first; break;
            case 7: geom[index] = scene.addHair(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,1.0f,2.0f,10,random_motion_vector(1.0f)).first; break;
            case 8: geom[index] = scene.addSubdivSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,4,4).first; break;
            case 9: geom[index] = scene.addSubdivSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,4,4,-1,random_motion_vector(1.0f)).first; break;
            case 10: 
              spheres[index] = Sphere(pos,2.0f);
              geom[index] = scene.addUserGeometryEmpty(sampler,RTC_BUILD_QUALITY_MEDIUM,&spheres[index]).first; break;
            }
            AssertNoError(device);
          }
          else { 
            rtcDetachGeometry(scene,geom[index]);     
            AssertNoError(device);
            geom[index] = -1; 
          }
        }
        rtcCommitScene(scene);
        AssertNoError(device);
        rtcCommitScene(scene);
        AssertNoError(device);
        if (i%2 == 0) std::cout << "." << std::flush;
      }
      
      /* now delete all geometries */
      for (size_t i=0; i<128; i++) 
        if (geom[i] != -1) rtcDetachGeometry(scene,geom[i]);
      rtcCommitScene(scene);
      AssertNoError(device);

      rtcCommitScene (scene);
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct UserGeometryIDTest : public VerifyApplication::Test
  {
    SceneFlags sflags;

    UserGeometryIDTest (std::string name, int isa, SceneFlags sflags)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,sflags);
      AssertNoError(device);

      int geom[128];
      for (size_t i=0; i<128; i++) geom[i] = -1;
      
      for (size_t i=0; i<size_t(50*state->intensity); i++) 
      {
        for (size_t j=0; j<10; j++) 
        {
          int index = random_int()%128;
          if (geom[index] == -1) {
            if (random_bool()) {
              RTCGeometry hgeom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
              rtcCommitGeometry(hgeom);
              unsigned int geomID = rtcAttachGeometry(scene,hgeom);
              rtcReleaseGeometry(hgeom);
              geom[geomID] = geomID;
              AssertNoError(device);
            } else {
              RTCGeometry hgeom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
              rtcCommitGeometry(hgeom);
              unsigned int geomID = rtcAttachGeometry(scene,hgeom);
              rtcReleaseGeometry(hgeom);
              geom[geomID] = geomID;
              AssertNoError(device);
            }
          } else {
             if (random_bool()) {
               rtcDetachGeometry(scene,geom[index]);     
               AssertNoError(device);
               geom[index] = -1; 
             }
          }
        }
        rtcCommitScene(scene);
        AssertNoError(device);
      }
      return VerifyApplication::PASSED;
    }
  };

  struct EnableDisableGeometryTest : public VerifyApplication::Test
  {
    SceneFlags sflags;

    EnableDisableGeometryTest (std::string name, int isa, SceneFlags sflags)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      RTCIntersectContext context;
      rtcInitIntersectContext(&context);
  
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      VerifyScene scene(device,sflags);
      AssertNoError(device);
      unsigned geom0 = scene.addSphere      (sampler,RTC_BUILD_QUALITY_MEDIUM,Vec3fa(-1,0,-1),1.0f,50).first;
      unsigned geom1 = scene.addQuadSphere  (sampler,RTC_BUILD_QUALITY_MEDIUM,Vec3fa(-1,0,+1),1.0f,50).first;
      unsigned geom2 = scene.addSubdivSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,Vec3fa(+1,0,-1),1.0f,5,4).first;
      unsigned geom3 = scene.addHair        (sampler,RTC_BUILD_QUALITY_MEDIUM,Vec3fa(+1,0,+1),1.0f,1.0f,1).first;
      RTCGeometry hgeom0 = rtcGetGeometry(scene,geom0);
      RTCGeometry hgeom1 = rtcGetGeometry(scene,geom1);
      RTCGeometry hgeom2 = rtcGetGeometry(scene,geom2);
      RTCGeometry hgeom3 = rtcGetGeometry(scene,geom3);
      AssertNoError(device);
      
      for (size_t i=0; i<17; i++)
      {
        bool enabled0 = i & 1, enabled1 = i & 2, enabled2 = i & 4, enabled3 = i & 8;
        if (enabled0) rtcEnableGeometry(hgeom0); else rtcDisableGeometry(hgeom0); AssertNoError(device);
        if (enabled1) rtcEnableGeometry(hgeom1); else rtcDisableGeometry(hgeom1); AssertNoError(device);
        if (enabled2) rtcEnableGeometry(hgeom2); else rtcDisableGeometry(hgeom2); AssertNoError(device);
        if (enabled3) rtcEnableGeometry(hgeom3); else rtcDisableGeometry(hgeom3); AssertNoError(device);
        rtcCommitScene (scene);
        AssertNoError(device);
        {
          RTCRayHit ray0 = makeRay(Vec3fa(-1,10,-1),Vec3fa(0,-1,0));
          RTCRayHit ray1 = makeRay(Vec3fa(-1,10,+1),Vec3fa(0,-1,0)); 
          RTCRayHit ray2 = makeRay(Vec3fa(+1,10,-1),Vec3fa(0,-1,0)); 
          RTCRayHit ray3 = makeRay(Vec3fa(+1,10,+1),Vec3fa(0,-1,0)); 
          rtcIntersect1(scene,&context,&ray0);
          rtcIntersect1(scene,&context,&ray1);
          rtcIntersect1(scene,&context,&ray2);
          rtcIntersect1(scene,&context,&ray3);
          bool ok0 = enabled0 ? ray0.hit.geomID == 0 : ray0.hit.geomID == RTC_INVALID_GEOMETRY_ID;
          bool ok1 = enabled1 ? ray1.hit.geomID == 1 : ray1.hit.geomID == RTC_INVALID_GEOMETRY_ID;
          bool ok2 = enabled2 ? ray2.hit.geomID == 2 : ray2.hit.geomID == RTC_INVALID_GEOMETRY_ID;
          bool ok3 = enabled3 ? ray3.hit.geomID == 3 : ray3.hit.geomID == RTC_INVALID_GEOMETRY_ID;
          if (!ok0 || !ok1 || !ok2 || !ok3) return VerifyApplication::FAILED;
        }
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };
  
  struct UpdateTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;

    UpdateTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}
    
    static void move_mesh(RTCGeometry mesh, size_t numVertices, Vec3fa& pos) 
    {
      Vec3ff* vertices = (Vec3ff*) rtcGetGeometryBufferData(mesh,RTC_BUFFER_TYPE_VERTEX,0);
      for (size_t i=0; i<numVertices; i++)
        vertices[i] += Vec3ff(pos,0.0f);
      rtcUpdateGeometryBuffer(mesh,RTC_BUFFER_TYPE_VERTEX,0);
      rtcCommitGeometry(mesh);
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;
      
      VerifyScene scene(device,sflags);
      AssertNoError(device);
      size_t numPhi = 10;
      size_t numVertices = 2*numPhi*(numPhi+1);
      Vec3fa pos0 = Vec3fa(-10,0,-10);
      Vec3fa pos1 = Vec3fa(-10,0,+10);
      Vec3fa pos2 = Vec3fa(+10,0,-10);
      Vec3fa pos3 = Vec3fa(+10,0,+10);
      unsigned geom0 = scene.addSphere      (sampler,quality,pos0,1.0f,numPhi).first;
      unsigned geom1 = scene.addQuadSphere  (sampler,quality,pos1,1.0f,numPhi).first;
      unsigned geom2 = scene.addSubdivSphere(sampler,quality,pos2,1.0f,numPhi,4).first;
      unsigned geom3 = scene.addSphereHair  (sampler,quality,pos3,1.0f).first;
      RTCGeometry hgeom0 = rtcGetGeometry(scene,geom0);
      RTCGeometry hgeom1 = rtcGetGeometry(scene,geom1);
      RTCGeometry hgeom2 = rtcGetGeometry(scene,geom2);
      RTCGeometry hgeom3 = rtcGetGeometry(scene,geom3);
      AssertNoError(device);
      
      for (size_t i=0; i<16; i++) 
      {
        bool move0 = i & 1, move1 = i & 2, move2 = i & 4, move3 = i & 8;
        Vec3fa ds(2,0.1f,2);
        if (move0) { move_mesh(hgeom0,numVertices,ds); pos0 += ds; }
        if (move1) { move_mesh(hgeom1,numVertices,ds); pos1 += ds; }
        if (move2) { move_mesh(hgeom2,numVertices,ds); pos2 += ds; }
        if (move3) { move_mesh(hgeom3,4,ds); pos3 += ds; }
        rtcCommitScene (scene);
        AssertNoError(device);

        RTCRayHit ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); // hits geomID == 0
        RTCRayHit ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); // hits geomID == 1
        RTCRayHit ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); // hits geomID == 2
        RTCRayHit ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); // hits geomID == 3
        RTCRayHit testRays[4] = { ray0, ray1, ray2, ray3 };

        const unsigned int maxRays = 100;
        RTCRayHit rays[maxRays];
        for (unsigned int numRays=1; numRays<maxRays; numRays++) {
          for (size_t i=0; i<numRays; i++) rays[i] = testRays[i%4];
          IntersectWithMode(imode,ivariant,scene,rays,numRays);
          for (size_t i=0; i<numRays; i++)
            if (ivariant & VARIANT_INTERSECT) {
              if (rays[i].hit.geomID == RTC_INVALID_GEOMETRY_ID)
                return VerifyApplication::FAILED;
            }
            else {
              if (rays[i].ray.tfar != float(neg_inf))
                return VerifyApplication::FAILED;
            }
        }
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct GarbageGeometryTest : public VerifyApplication::Test
  {
    GarbageGeometryTest (std::string name, int isa)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + "isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      for (size_t i=0; i<size_t(1000*state->intensity); i++)
      {
        RandomSampler_init(sampler,int(i*23565));
        if (i%20 == 0) std::cout << "." << std::flush;

        SceneFlags sflags = getSceneFlags(i); 
        VerifyScene scene(device,sflags);
        AssertNoError(device);
        
        for (size_t j=0; j<20; j++) 
        {
          size_t numPrimitives = random_int()%256;
          switch (random_int()%10) {
          case 0: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageTriangleMesh(random_int(),numPrimitives,false)); break;
          case 1: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageTriangleMesh(random_int(),numPrimitives,true )); break;
          case 2: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageQuadMesh(random_int(),numPrimitives,false)); break;
          case 3: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageQuadMesh(random_int(),numPrimitives,true )); break;
          case 4: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageGridMesh(random_int(),numPrimitives,false)); break;
          case 5: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageGridMesh(random_int(),numPrimitives,true )); break;
          case 6: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageHair(random_int(),numPrimitives,false)); break;
          case 7: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageHair(random_int(),numPrimitives,true )); break;
          case 8: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageLineSegments(random_int(),numPrimitives,false)); break;
          case 9: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageLineSegments(random_int(),numPrimitives,true )); break;
          case 10: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbagePointSet(random_int(),numPrimitives,false)); break;
          case 11: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbagePointSet(random_int(),numPrimitives,true )); break;
            //case 12: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageSubdivMesh(random_int(),numPrimitives,false)); break; // FIXME: not working yet
            //case 13: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGarbageSubdivMesh(random_int(),numPrimitives,true )); break;
          }
          AssertNoError(device);
        }
        
        rtcCommitScene(scene);
        AssertNoError(device);
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  const size_t num_interpolation_vertices = 16;
  const size_t num_interpolation_quad_faces = 9;
  const size_t num_interpolation_triangle_faces = 18;

  __aligned(16) float interpolation_vertices[num_interpolation_vertices*3] = {
    -1.0f, -1.0f, 0.0f,
    0.0f, -1.0f, 0.0f, 
    +1.0f, -1.0f, 0.0f,
    +2.0f, -1.0f, 0.0f,
    
    -1.0f,  0.0f, 0.0f,
    0.0f,  0.0f, 0.0f,
    +1.0f,  0.0f, 0.0f,
    +2.0f,  0.0f, 0.0f,

    -1.0f, +1.0f, 0.0f,
    0.0f, +1.0f, 0.0f,
    +1.0f, +1.0f, 0.0f,
    +2.0f, +1.0f, 0.0f,

    -1.0f, +2.0f, 0.0f,
    0.0f, +2.0f, 0.0f,
    +1.0f, +2.0f, 0.0f,
    +2.0f, +2.0f, 0.0f,
  };

  __aligned(16) int interpolation_quad_indices[num_interpolation_quad_faces*4] = {
    0, 1, 5, 4,
    1, 2, 6, 5,
    2, 3, 7, 6,
    4, 5, 9, 8, 
    5, 6, 10, 9,
    6, 7, 11, 10,
    8, 9, 13, 12,
    9, 10, 14, 13,
    10, 11, 15, 14
  };

  __aligned(16) int interpolation_triangle_indices[num_interpolation_triangle_faces*3] = {
    0, 1, 5,  0, 5, 4,
    1, 2, 6,  1, 6, 5,
    2, 3, 7,  2, 7, 6,
    4, 5, 9,  4, 9, 8, 
    5, 6, 10,  5, 10, 9,
    6, 7, 11,  6, 11, 10,
    8, 9, 13,   8, 13, 12,
    9, 10, 14,   9, 14, 13,
    10, 11, 15,  10, 15, 14
  };

  __aligned(16) int interpolation_quad_faces[num_interpolation_quad_faces] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4
  };

  __aligned(16) float interpolation_vertex_crease_weights[2] = {
    inf, inf
  };

  __aligned(16) unsigned int interpolation_vertex_crease_indices[2] = {
    12, 15
  };

  __aligned(16) float interpolation_edge_crease_weights[3] = {
    inf, inf, inf
  };

  __aligned(16) unsigned int interpolation_edge_crease_indices[6] = 
  {
    8,9, 9,10, 10,11
  };

  struct InterpolateSubdivTest : public VerifyApplication::Test
  {
    unsigned int N;
    
    InterpolateSubdivTest (std::string name, int isa, unsigned int N)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), N(N) {}

    bool checkInterpolation2D(RTCGeometry geom, int primID, float u, float v, int v0, RTCBufferType bufferType, unsigned int bufferSlot, float* data, unsigned int N, unsigned int N_total)
    {
      assert(N < 256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,N);
      
      for (size_t i=0; i<N; i++) {
        float p0 = (1.0f/6.0f)*(1.0f*data[(v0-4-1)*N_total+i] + 4.0f*data[(v0-4+0)*N_total+i] + 1.0f*data[(v0-4+1)*N_total+i]);
        float p1 = (1.0f/6.0f)*(1.0f*data[(v0+0-1)*N_total+i] + 4.0f*data[(v0+0+0)*N_total+i] + 1.0f*data[(v0+0+1)*N_total+i]);
        float p2 = (1.0f/6.0f)*(1.0f*data[(v0+4-1)*N_total+i] + 4.0f*data[(v0+4+0)*N_total+i] + 1.0f*data[(v0+4+1)*N_total+i]);
        float p = (1.0f/6.0f)*(1.0f*p0+4.0f*p1+1.0f*p2);
        passed &= fabsf(p-P[i]) < 1E-4f;
      }
      return passed;
    }
    
    bool checkInterpolation1D(RTCGeometry geom, int primID, float u, float v, int v0, int v1, int v2, RTCBufferType bufferType, unsigned int bufferSlot, float* data, unsigned int N, unsigned int N_total)
    {
      assert(N < 256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,(unsigned int)N);
      
      for (size_t i=0; i<N; i++) {
        float v = (1.0f/6.0f)*(1.0f*data[v0*N_total+i] + 4.0f*data[v1*N_total+i] + 1.0f*data[v2*N_total+i]);
        passed &= fabsf(v-P[i]) < 0.001f;
      }
      return passed;
    }
    
    bool checkInterpolationSharpVertex(RTCGeometry geom, int primID, float u, float v, int v0, RTCBufferType bufferType, unsigned int bufferSlot, float* data, size_t N, size_t N_total)
    {
      assert(N < 256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,(unsigned int)N);
      
      for (size_t i=0; i<N; i++) {
        float v = data[v0*N_total+i];
        passed &= fabs(v-P[i]) < 1E-3f;
      }
      return passed;
    }
    
    bool checkSubdivInterpolation(const RTCDeviceRef& device, RTCGeometry geom, RTCBufferType bufferType, unsigned int bufferSlot, float* vertices0, unsigned int N, unsigned int N_total)
    {
      rtcSetGeometrySubdivisionMode(geom,0,RTC_SUBDIVISION_MODE_SMOOTH_BOUNDARY);
      AssertNoError(device);
      rtcCommitGeometry(geom);
      AssertNoError(device);
      bool passed = true;
      passed &= checkInterpolation1D(geom,0,0.0f,0.0f,4,0,1,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkInterpolation1D(geom,2,1.0f,0.0f,2,3,7,bufferType,bufferSlot,vertices0,N,N_total);
      
      passed &= checkInterpolation2D(geom,3,1.0f,0.0f,5,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkInterpolation2D(geom,1,1.0f,1.0f,6,bufferType,bufferSlot,vertices0,N,N_total);
      
      //passed &= checkInterpolation1D(geom,3,1.0f,1.0f,8,9,10,bufferType,bufferSlot,vertices0,N,N_total);
      //passed &= checkInterpolation1D(geom,7,1.0f,0.0f,9,10,11,bufferType,bufferSlot,vertices0,N,N_total);
      
      passed &= checkInterpolationSharpVertex(geom,6,0.0f,1.0f,12,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkInterpolationSharpVertex(geom,8,1.0f,1.0f,15,bufferType,bufferSlot,vertices0,N,N_total);
      
      rtcSetGeometrySubdivisionMode(geom,0,RTC_SUBDIVISION_MODE_PIN_CORNERS);
      rtcCommitGeometry(geom);
      AssertNoError(device);
      
      passed &= checkInterpolationSharpVertex(geom,0,0.0f,0.0f,0,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkInterpolationSharpVertex(geom,2,1.0f,0.0f,3,bufferType,bufferSlot,vertices0,N,N_total);
      return passed;
    }
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      size_t M = num_interpolation_vertices*N+16; // padds the arrays with some valid data
      
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SUBDIVISION);
      AssertNoError(device);
      rtcSetGeometryVertexAttributeCount(geom,2);
      
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX,                0, RTC_FORMAT_UINT,  interpolation_quad_indices,          0, sizeof(unsigned int),   num_interpolation_quad_faces*4);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_FACE,                 0, RTC_FORMAT_UINT,  interpolation_quad_faces,            0, sizeof(unsigned int),   num_interpolation_quad_faces);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_EDGE_CREASE_INDEX,    0, RTC_FORMAT_UINT2, interpolation_edge_crease_indices,   0, 2*sizeof(unsigned int), 3);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_EDGE_CREASE_WEIGHT,   0, RTC_FORMAT_FLOAT, interpolation_edge_crease_weights,   0, sizeof(float),          3);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_CREASE_INDEX,  0, RTC_FORMAT_UINT,  interpolation_vertex_crease_indices, 0, sizeof(unsigned int),   2);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_CREASE_WEIGHT, 0, RTC_FORMAT_FLOAT, interpolation_vertex_crease_weights, 0, sizeof(float),          2);
      AssertNoError(device);
      
      std::vector<float> vertices0(M);
      for (size_t i=0; i<M; i++) vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices0.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      /*std::vector<float> vertices1(M);
        for (size_t i=0; i<M; i++) vertices1[i] = random_float();
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 1, RTC_FORMAT_FLOAT3, vertices1.data(), 0, N*sizeof(float), num_interpolation_vertices);
        AssertNoError(device);*/
      
      std::vector<float> user_vertices0(M);
      for (size_t i=0; i<M; i++) user_vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices0.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      std::vector<float> user_vertices1(M);
      for (size_t i=0; i<M; i++) user_vertices1[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices1.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      bool passed = true;
      if (N >= 3) {
        passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),3,N);
        //passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),3,N);
      }
      passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),N,N);
      passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),N,N);
      
      passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),1,N);
      //passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),1,N);
      passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),1,N);
      passed &= checkSubdivInterpolation(device,geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),1,N);

      rtcReleaseGeometry(geom);
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  struct InterpolateTrianglesTest : public VerifyApplication::Test
  {
    size_t N;
    
    InterpolateTrianglesTest (std::string name, int isa, size_t N)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), N(N) {}
    
    bool checkTriangleInterpolation(RTCGeometry geom, int primID, float u, float v, int v0, int v1, int v2, RTCBufferType bufferType, unsigned int bufferSlot, float* data, size_t N, size_t N_total)
    {
      assert(N<256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,(unsigned int)N);
      
      for (size_t i=0; i<N; i++) {
        float p0 = data[v0*N_total+i];
        float p1 = data[v1*N_total+i];
        float p2 = data[v2*N_total+i];
        float p = (1.0f-u-v)*p0 + u*p1 + v*p2;
        passed &= fabs(p-P[i]) < 1E-4f;
      }
      return passed;
    }
    
    bool checkTriangleInterpolation(RTCGeometry geom, RTCBufferType bufferType, unsigned int bufferSlot, float* vertices0, size_t N, size_t N_total)
    {
      bool passed = true;
      passed &= checkTriangleInterpolation(geom,0,0.0f,0.0f,0,1,5,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkTriangleInterpolation(geom,0,0.5f,0.5f,0,1,5,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkTriangleInterpolation(geom,17,0.0f,0.0f,10,15,14,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkTriangleInterpolation(geom,17,0.5f,0.5f,10,15,14,bufferType,bufferSlot,vertices0,N,N_total);
      return passed;
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      size_t M = num_interpolation_vertices*N+16; // padds the arrays with some valid data
      
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
      AssertNoError(device);
      rtcSetGeometryVertexAttributeCount(geom,2);
      
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, interpolation_triangle_indices, 0, 3*sizeof(unsigned int), num_interpolation_triangle_faces*3);
      AssertNoError(device);
      
      std::vector<float> vertices0(M);
      for (size_t i=0; i<M; i++) vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices0.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      /*std::vector<float> vertices1(M);
        for (size_t i=0; i<M; i++) vertices1[i] = random_float();
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 1, RTC_FORMAT_FLOAT3, vertices1.data(), 0, N*sizeof(float), num_interpolation_vertices);
        AssertNoError(device);*/
      
      std::vector<float> user_vertices0(M);
      for (size_t i=0; i<M; i++) user_vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices0.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      std::vector<float> user_vertices1(M);
      for (size_t i=0; i<M; i++) user_vertices1[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices1.data(), 0, N*sizeof(float), num_interpolation_vertices);
      AssertNoError(device);
      
      rtcDisableGeometry(geom);
      AssertNoError(device);
      rtcCommitGeometry(geom);
      AssertNoError(device);
      
      bool passed = true;
      if (N >= 3) {
        passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),3,N);
        //passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),3,N);
      }
      passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),N,N);
      passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),N,N);
      
      passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),1,N);
      //passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),1,N);
      passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),1,N);
      passed &= checkTriangleInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),1,N);

      rtcReleaseGeometry(geom);
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  __aligned(16) RTCGrid interpolation_grids[1];
  
  struct InterpolateGridTest : public VerifyApplication::Test
  {
    size_t N;
    
    InterpolateGridTest (std::string name, int isa, size_t N)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), N(N) {}
    
    bool checkGridInterpolation(RTCGeometry geom, int primID, float u, float v, RTCBufferType bufferType, unsigned int bufferSlot, size_t N)
    {
      assert(N<256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,(unsigned int)N);
      
      for (size_t i=0; i<N; i++)
      {
        float p = (2.0f*u+3.0f*v)+float(i);
        passed &= fabs(p-P[i]) < 1E-4f;
      }
      return passed;
    }
    
    bool checkGridInterpolation(RTCGeometry geom, RTCBufferType bufferType, unsigned int bufferSlot, size_t N)
    {
      bool passed = true;
      for (size_t i=0; i<100; i++) {
        const float u = random_float();
        const float v = random_float();
        passed &= checkGridInterpolation(geom,0,u,v,bufferType,bufferSlot,N);
      }
      return passed;
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      size_t M = 16*N+16; // padds the arrays with some valid data
      
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_GRID);
      AssertNoError(device);

      interpolation_grids[0].startVertexID = 0;
      interpolation_grids[0].stride = 4;
      interpolation_grids[0].width = 4;
      interpolation_grids[0].height = 4;
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_GRID, 0, RTC_FORMAT_GRID, interpolation_grids, 0, sizeof(RTCGrid), 1);
      AssertNoError(device);
      
      std::vector<float> vertices0(M);
      for (size_t y=0; y<4; y++)
        for (size_t x=0; x<4; x++)
          for (size_t i=0; i<N; i++)
            vertices0[(y*4+x)*N+i] = (2.0f*float(x)/3.0f + 3.0f*y/3.0f)+float(i);
      
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices0.data(), 0, N*sizeof(float), 4*4);
      AssertNoError(device);
      
      rtcCommitGeometry(geom);
      AssertNoError(device);
      
      bool passed = true;
      passed &= checkGridInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,0,N);
      rtcReleaseGeometry(geom);
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };
  
  const size_t num_interpolation_hair_vertices = 13;
  const size_t num_interpolation_hairs = 4;

  __aligned(16) int interpolation_hair_indices[num_interpolation_hairs] = {
    0, 3, 6, 9
  };

  struct InterpolateHairTest : public VerifyApplication::Test
  {
    size_t N;
    
    InterpolateHairTest (std::string name, int isa, size_t N)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), N(N) {}
    
    bool checkHairInterpolation(RTCGeometry geom, int primID, float u, float v, int v0, RTCBufferType bufferType, unsigned int bufferSlot, float* data, size_t N, size_t N_total)
    {
      assert(N<256);
      bool passed = true;
      float P[256], dPdu[256], dPdv[256];
      rtcInterpolate1(geom,primID,u,v,bufferType,bufferSlot,P,dPdu,dPdv,(unsigned int)N);
      
      for (size_t i=0; i<N; i++) {
        const float p00 = data[(v0+0)*N_total+i];
        const float p01 = data[(v0+1)*N_total+i];
        const float p02 = data[(v0+2)*N_total+i];
        const float p03 = data[(v0+3)*N_total+i];
        const float t0 = 1.0f - u, t1 = u;
        const float p10 = p00 * t0 + p01 * t1;
        const float p11 = p01 * t0 + p02 * t1;
        const float p12 = p02 * t0 + p03 * t1;
        const float p20 = p10 * t0 + p11 * t1;
        const float p21 = p11 * t0 + p12 * t1;
        const float p30 = p20 * t0 + p21 * t1;
        passed &= fabs(p30-P[i]) < 1E-4f;
      }
      return passed;
    }
    
    bool checkHairInterpolation(RTCGeometry geom, RTCBufferType bufferType, unsigned int bufferSlot, float* vertices0, size_t N, size_t N_total)
    {
      bool passed = true;
      passed &= checkHairInterpolation(geom,0,0.0f,0.0f,0,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkHairInterpolation(geom,1,0.5f,0.0f,3,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkHairInterpolation(geom,2,0.0f,0.0f,6,bufferType,bufferSlot,vertices0,N,N_total);
      passed &= checkHairInterpolation(geom,3,0.2f,0.0f,9,bufferType,bufferSlot,vertices0,N,N_total);
      return passed;
    }
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      size_t M = num_interpolation_hair_vertices*N+16; // padds the arrays with some valid data
      
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_BEZIER_CURVE);
      AssertNoError(device);
      rtcSetGeometryVertexAttributeCount(geom,2);
      
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, interpolation_hair_indices, 0, sizeof(unsigned int), num_interpolation_hairs);
      AssertNoError(device);
      
      std::vector<float> vertices0(M);
      for (size_t i=0; i<M; i++) vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, vertices0.data(), 0, N*sizeof(float), num_interpolation_hair_vertices);
      AssertNoError(device);
      
      /*std::vector<float> vertices1(M);
        for (size_t i=0; i<M; i++) vertices1[i] = random_float();
        rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 1, RTC_FORMAT_FLOAT4, vertices1.data(), 0, N*sizeof(float), num_interpolation_hair_vertices);
        AssertNoError(device);*/
      
      std::vector<float> user_vertices0(M);
      for (size_t i=0; i<M; i++) user_vertices0[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices0.data(), 0, N*sizeof(float), num_interpolation_hair_vertices);
      AssertNoError(device);
      
      std::vector<float> user_vertices1(M);
      for (size_t i=0; i<M; i++) user_vertices1[i] = random_float();
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, RTCFormat(RTC_FORMAT_FLOAT+N), user_vertices1.data(), 0, N*sizeof(float), num_interpolation_hair_vertices);
      AssertNoError(device);
      
      rtcDisableGeometry(geom);
      AssertNoError(device);
      rtcCommitGeometry(geom);
      AssertNoError(device);
      
      bool passed = true;
      if (N >= 4) {
        passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),4,N);
        //passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),4,N);
      }
      passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),N,N);
      passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),N,N);
      
      passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,0,vertices0.data(),1,N);
      //passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX,1,vertices1.data(),1,N);
      passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,0,user_vertices0.data(),1,N);
      passed &= checkHairInterpolation(geom,RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE,1,user_vertices1.data(),1,N);

      rtcReleaseGeometry(geom);
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  struct TriangleHitTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags; 
    RTCBuildQuality quality; 

    TriangleHitTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}

    inline Vec3fa uniformSampleTriangle(const Vec3fa &a, const Vec3fa &b, const Vec3fa &c, float &u, float& v)
    {
      const float su = sqrtf(u);
      v *= su;
      u = 1.0f-su;
      if (u < 0.001f || v < 0.001f || (u+v) > 0.999f) { u = 0.333f; v = 0.333f; } // avoid hitting edges
      return c + u * (a-c) + v * (b-c);
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;
     
      Vec3f vertices[4] = {
        Vec3f(0.0f,0.0f,0.0f),
        Vec3f(1.0f,0.0f,0.0f),
        Vec3f(0.0f,1.0f,0.0f),
        Vec3f(zero) // dummy vertex for 16 byte padding
      };
      Triangle triangles[1] = {
        Triangle(0,1,2)
      };
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);
      
      RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom,quality);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices , 0, sizeof(Vec3f), 3);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX , 0, RTC_FORMAT_UINT3,  triangles, 0, sizeof(Triangle), 1);
      rtcCommitGeometry(geom);
      rtcAttachGeometry(scene,geom);
      rtcReleaseGeometry(geom);
      rtcCommitScene (scene);
      AssertNoError(device);

      float u[256], v[256];
      RTCRayHit rays[256];
      for (size_t i=0; i<256; i++)
      {
        u[i] = random_float(); 
        v[i] = random_float();
        Vec3fa from(0.0f,0.0f,-1.0f);
        Vec3fa to = uniformSampleTriangle(vertices[1],vertices[2],vertices[0],u[i],v[i]);
        rays[i] = makeRay(from,to-from);
      }
      IntersectWithMode(imode,ivariant,scene,rays,256);

      for (size_t i=0; i<256; i++)
      {
        
        if (!(ivariant & VARIANT_INTERSECT)) 
        {
          if (rays[i].ray.tfar != float(neg_inf)) return VerifyApplication::FAILED;          
          continue;
        }


        if (rays[i].hit.geomID != 0) return VerifyApplication::FAILED;
        if (rays[i].hit.primID != 0) return VerifyApplication::FAILED;
        if (abs(rays[i].hit.u - u[i]) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        if (abs(rays[i].hit.v - v[i]) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        if (abs(rays[i].ray.tfar - 1.0f) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        const Vec3fa org(rays[i].ray.org_x,rays[i].ray.org_y,rays[i].ray.org_z);
        const Vec3fa dir(rays[i].ray.dir_x,rays[i].ray.dir_y,rays[i].ray.dir_z);
        const Vec3fa ht  = org + rays[i].ray.tfar*dir;
        const Vec3fa huv = vertices[0] + rays[i].hit.u*(vertices[1]-vertices[0]) + rays[i].hit.v*(vertices[2]-vertices[0]);
        if (reduce_max(abs(ht-huv)) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        const Vec3fa Ng = Vec3fa(rays[i].hit.Ng_x,rays[i].hit.Ng_y,rays[i].hit.Ng_z);
        if (reduce_max(abs(Ng - Vec3fa(0.0f,0.0f,1.0f))) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct QuadHitTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags; 
    RTCBuildQuality quality; 

    QuadHitTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;
     
      Vec3f vertices[5] = {
        Vec3f(0.0f,0.0f,0.0f),
        Vec3f(1.0f,0.0f,0.0f),
        Vec3f(1.0f,1.0f,0.0f),
        Vec3f(0.0f,1.0f,0.0f),
        Vec3f(zero) // dummy vertex for 16 byte padding
      };
      int quads[4] = {
        0,1,2,3
      };
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);
      
      RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_QUAD);
      rtcSetGeometryBuildQuality(geom, quality);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices , 0, sizeof(Vec3f), 4);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX , 0, RTC_FORMAT_UINT4,  quads, 0, 4*sizeof(int), 1);
      rtcCommitGeometry(geom);
      rtcAttachGeometry(scene,geom);
      rtcReleaseGeometry(geom);
      rtcCommitScene (scene);
      AssertNoError(device);

      float u[256], v[256];
      RTCRayHit rays[256];
      for (size_t i=0; i<256; i++)
      {
        u[i] = random_float(); 
        v[i] = random_float();
        if (u[i] < 0.001f || v[i] < 0.001f || u[i] > 0.999f || v[i] > 0.999f) { u[i] = 0.333f; v[i] = 0.333f; } // avoid hitting edges
        Vec3fa from(0.0f,0.0f,-1.0f);
        Vec3fa to = vertices[0] + u[i]*(vertices[1]-vertices[0]) + v[i]*(vertices[3]-vertices[0]);
        rays[i] = makeRay(from,to-from);
      }
      IntersectWithMode(imode,ivariant,scene,rays,256);

      for (size_t i=0; i<256; i++)
      {
        if (!(ivariant & VARIANT_INTERSECT))
        {
          if (rays[i].ray.tfar != float(neg_inf)) return VerifyApplication::FAILED;          
          continue;
        }
        if (rays[i].hit.primID != 0) return VerifyApplication::FAILED;
        if (abs(rays[i].hit.u - u[i]) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        if (abs(rays[i].hit.v - v[i]) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        if (abs(rays[i].ray.tfar - 1.0f) > 16.0f*float(ulp)) return VerifyApplication::FAILED;

        const Vec3fa org(rays[i].ray.org_x,rays[i].ray.org_y,rays[i].ray.org_z);
        const Vec3fa dir(rays[i].ray.dir_x,rays[i].ray.dir_y,rays[i].ray.dir_z);
        const Vec3fa ht  = org + rays[i].ray.tfar*dir;
        const Vec3fa huv = vertices[0] + rays[i].hit.u*(vertices[1]-vertices[0]) + rays[i].hit.v*(vertices[3]-vertices[0]);
        if (reduce_max(abs(ht-huv)) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
        const Vec3fa Ng = Vec3fa(rays[i].hit.Ng_x,rays[i].hit.Ng_y,rays[i].hit.Ng_z);
        if (reduce_max(abs(Ng - Vec3fa(0.0f,0.0f,1.0f))) > 16.0f*float(ulp)) return VerifyApplication::FAILED;
      }
      AssertNoError(device);
      
      return VerifyApplication::PASSED;
    }
  };
  
  struct RayMasksTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags; 
    RTCBuildQuality quality; 

    RayMasksTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      bool passed = true;
      Vec3fa pos0 = Vec3fa(-10,0,-10);
      Vec3fa pos1 = Vec3fa(-10,0,+10);
      Vec3fa pos2 = Vec3fa(+10,0,-10);
      Vec3fa pos3 = Vec3fa(+10,0,+10);
      
      VerifyScene scene(device,sflags);
      unsigned int geom0 = scene.addSphere      (sampler,quality,pos0,1.0f,50).first;
      unsigned int geom1 = scene.addQuadSphere  (sampler,quality,pos1,1.0f,50).first; 
      unsigned int geom2 = scene.addSubdivSphere(sampler,quality,pos2,1.0f,5,4).first;
      unsigned int geom3 = scene.addHair        (sampler,quality,pos3,1.0f,1.0f,1).first;
      RTCGeometry hgeom0 = rtcGetGeometry(scene,geom0);
      RTCGeometry hgeom1 = rtcGetGeometry(scene,geom1);
      RTCGeometry hgeom2 = rtcGetGeometry(scene,geom2);
      RTCGeometry hgeom3 = rtcGetGeometry(scene,geom3);
      rtcSetGeometryMask(hgeom0,1);
      rtcSetGeometryMask(hgeom1,2);
      rtcSetGeometryMask(hgeom2,4);
      rtcSetGeometryMask(hgeom3,8);
      rtcCommitScene (scene);
      AssertNoError(device);
      
      for (unsigned i=0; i<16; i++) 
      {
        unsigned mask0 = i;
        unsigned mask1 = i+1;
        unsigned mask2 = i+2;
        unsigned mask3 = i+3;
        unsigned masks[4] = { mask0, mask1, mask2, mask3 };
        RTCRayHit ray0 = makeRay(pos0+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray0.ray.mask = mask0;
        RTCRayHit ray1 = makeRay(pos1+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray1.ray.mask = mask1;
        RTCRayHit ray2 = makeRay(pos2+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray2.ray.mask = mask2;
        RTCRayHit ray3 = makeRay(pos3+Vec3fa(0,10,0),Vec3fa(0,-1,0)); ray3.ray.mask = mask3;
        RTCRayHit rays[4] = { ray0, ray1, ray2, ray3 };
        IntersectWithMode(imode,ivariant,scene,rays,4);
        for (size_t j=0; j<4; j++)
          passed &= masks[j] & (1<<j) ? rays[j].hit.geomID != RTC_INVALID_GEOMETRY_ID : rays[j].hit.geomID == RTC_INVALID_GEOMETRY_ID;
      }
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  struct BackfaceCullingTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;
    GeometryType gtype;

    BackfaceCullingTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, GeometryType gtype, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality), gtype(gtype) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;
       
      /* create triangle that is front facing for a right handed 
         coordinate system if looking along the z direction */
      VerifyScene scene(device,sflags);
      AssertNoError(device);
      const Vec3fa p0 = Vec3fa(0.0f);
      const Vec3fa dx = Vec3fa(0.0f,1.0f,0.0f);
      const Vec3fa dy = Vec3fa(1.0f,0.0f,0.0f);
      switch (gtype) {
      case TRIANGLE_MESH:    scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTrianglePlane(p0,dx,dy,1,1)); break;
      case TRIANGLE_MESH_MB: scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTrianglePlane(p0,dx,dy,1,1)->set_motion_vector(zero)); break;
      case QUAD_MESH:        scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadPlane(p0,dx,dy,1,1)); break;
      case QUAD_MESH_MB:     scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadPlane(p0,dx,dy,1,1)->set_motion_vector(zero)); break;
      case GRID_MESH:        scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridPlane(p0,dx,dy,1,1)); break;
      case GRID_MESH_MB:     scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridPlane(p0,dx,dy,1,1)->set_motion_vector(zero)); break;
      case SUBDIV_MESH:      scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createSubdivPlane(p0,dx,dy,1,1,4.0f)); break;
      case SUBDIV_MESH_MB:   scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createSubdivPlane(p0,dx,dy,1,1,4.0f)->set_motion_vector(zero)); break;
      default:               throw std::runtime_error("unsupported geometry type: "+to_string(gtype)); 
      }
      
      AssertNoError(device);
      rtcCommitScene (scene);
      AssertNoError(device);

      const size_t numRays = 1000;
      RTCRayHit rays[numRays];
      bool passed = true;

      for (size_t i=0; i<numRays; i++) {
        const float rx = random_float();
        const float ry = random_float();
        if (i%2) rays[i] = makeRay(Vec3fa(rx,ry,+1),Vec3fa(0,0,-1)); 
        else     rays[i] = makeRay(Vec3fa(rx,ry,-1),Vec3fa(0,0,+1)); 
      }
      
      IntersectWithMode(imode,ivariant,scene,rays,numRays);
      
      for (size_t i=0; i<numRays; i++) 
      {
        Vec3fa dir(rays[i].ray.dir_x,rays[i].ray.dir_y,rays[i].ray.dir_z);
        Vec3fa Ng (rays[i].hit.Ng_x, rays[i].hit.Ng_y, rays[i].hit.Ng_z);
        if (i%2) passed &= rays[i].hit.geomID == RTC_INVALID_GEOMETRY_ID;
        else {
          passed &= rays[i].hit.geomID == 0;
          if (ivariant & VARIANT_INTERSECT)
            passed &= dot(dir,Ng) < 0.0f;
        }
      }
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  struct IntersectionFilterTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;
    bool subdiv;

    IntersectionFilterTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, bool subdiv, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality), subdiv(subdiv) {}
    
    static void intersectionFilterN(const RTCFilterFunctionNArguments* const args)
    {
      if ((size_t)args->geometryUserPtr != 123) 
        return;

      for (unsigned int i=0; i<args->N; i++)
      {
	if (args->valid[i] != -1) continue;

        /* reject hit */
        if (RTCHitN_primID(args->hit,args->N,i) & 2) {
          args->valid[i] = 0;
        }
      }
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      VerifyScene scene(device,sflags);
      Vec3fa p0(-0.75f,-0.25f,-10.0f), dx(4,0,0), dy(0,4,0);
      unsigned int geomID0 = 0;
      if (subdiv) geomID0 = scene.addSubdivPlane (sampler,quality, 4, p0, dx, dy).first;
      else        geomID0 = scene.addPlane       (sampler,quality, 4, p0, dx, dy).first;
      RTCGeometry geom0 = rtcGetGeometry(scene,geomID0);
      rtcSetGeometryUserData(geom0,(void*)123);
      rtcSetGeometryIntersectFilterFunction (geom0,intersectionFilterN);
      rtcSetGeometryOccludedFilterFunction (geom0,intersectionFilterN);
      rtcCommitScene (scene);
      AssertNoError(device);
      
      RTCRayHit rays[16];
      for (unsigned int iy=0; iy<4; iy++) 
      {
        for (unsigned int ix=0; ix<4; ix++) 
        {
          unsigned int primID = iy*4+ix;
          if (!subdiv) primID *= 2;
          rays[iy*4+ix] = makeRay(Vec3fa(float(ix),float(iy),0.0f),Vec3fa(0,0,-1));
        }
      }
      IntersectWithMode(imode,ivariant,scene,rays,16);
      
      bool passed = true;
      for (unsigned int iy=0; iy<4; iy++) 
      {
        for (unsigned int ix=0; ix<4; ix++) 
        {
          unsigned int primID = iy*4+ix;
          if (!subdiv) primID *= 2;
          RTCRayHit& ray = rays[iy*4+ix];
          bool ok = true;
          if (ivariant & VARIANT_INTERSECT)
            ok = (primID & 2) ? (ray.hit.geomID == RTC_INVALID_GEOMETRY_ID) : (ray.hit.geomID == 0);
          else
            ok = ((primID & 2) != 0) == (ray.ray.tfar != float(neg_inf));
          if (!ok) passed = false;
        }
      }
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };

  struct InstancingTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;
    bool subdiv;

    InstancingTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, bool subdiv, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality), subdiv(subdiv) {
      }

    struct IntersectContext {
      RTCIntersectContext context;
      int numHits[16];
    };
    
    static void intersectFilter(const RTCFilterFunctionNArguments* args)
    {
      assert(args);
      assert(args->context);

      IntersectContext* context = (IntersectContext*)(args->context);

      for (unsigned int i=0; i<args->N; i++) 
      {
        const unsigned int rayId = RTCRayN_id(args->ray, args->N, i);
        if(args->valid[i] && rayId >= 16)
          throw std::runtime_error("Invalid ray id in intersection filter.");
        if (args->valid[i])
        {
          assert(rayId < 16);
          context->numHits[rayId] += 1;
        }
      }
    }

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      Ref<SceneGraph::Node> child;
      Ref<SceneGraph::Node> parent;
      if (subdiv)
        child = SceneGraph::createSubdivSphere(Vec3fa(0.f), 1.f, 32, 2.f);
      else
        child = SceneGraph::createQuadSphere(Vec3fa(0.f), 1.f, 32);

      for (int i = 0; i < RTC_MAX_INSTANCE_LEVEL_COUNT; ++i)
      { 
        parent = new SceneGraph::TransformNode(AffineSpace3fa(one), child);
        child = parent;
      }

      sflags.sflags = sflags.sflags | RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION;
      IntersectContext ctx;
      rtcInitIntersectContext(&ctx.context);
      ctx.context.filter = intersectFilter;

      VerifyScene scene(device, sflags);
      scene.addGeometry(quality, parent);
      rtcCommitScene(scene);
      AssertNoError(device);

      RTCRayHit rays[16];
      for (unsigned int iy=0; iy<4; iy++) 
      {
        for (unsigned int ix=0; ix<4; ix++) 
        {
          const int id = iy*4+ix;
          rays[id] = makeRay(Vec3fa(float(ix)/10.f,float(iy)/10.f,-2.0f),Vec3fa(0,0,1));
          rays[id].ray.id = id;
          ctx.numHits[id] = 0;
        }
      }
      IntersectWithMode(imode,ivariant,scene,rays,16,&ctx.context);
      bool passed = true;
      for (unsigned int iy=0; iy<4; iy++) 
      {
        for (unsigned int ix=0; ix<4; ix++) 
        {
          const unsigned id = iy*4+ix;
          passed &= (ctx.numHits[id] > 0);
          RTCRayHit& ray = rays[id];
          if (ivariant & VARIANT_INTERSECT)
            passed &= (ray.hit.geomID != RTC_INVALID_GEOMETRY_ID);
          else
            passed &= (ray.ray.tfar == (float)neg_inf);
        }
      }
      assert(passed);
      AssertNoError(device);

      return (VerifyApplication::TestReturnValue) passed;
    }
  };
    
  struct InactiveRaysTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;

    static const size_t N = 10;
    static const size_t maxStreamSize = 100;
    
    InactiveRaysTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}
   
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      Vec3fa pos = zero;
      VerifyScene scene(device,sflags);
      scene.addSphere(sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,50); // FIXME: use different geometries too
      rtcCommitScene (scene);
      AssertNoError(device);

      RTCRayHit invalid_ray; clearRay(invalid_ray);
      invalid_ray.ray.tnear = pos_inf;
      invalid_ray.ray.tfar  = 0.0f;
      
      size_t numFailures = 0;
      for (size_t i=0; i<size_t(N*state->intensity); i++) 
      {
        for (unsigned int M=1; M<maxStreamSize; M++)
        {
          bool valid[maxStreamSize];
          __aligned(16) RTCRayHit rays[maxStreamSize];
          for (unsigned int j=0; j<M; j++) 
          {
            if (rand()%2) {
              valid[j] = true;
              Vec3fa org = 2.0f*random_Vec3fa()-Vec3fa(1.0f);
              Vec3fa dir = 2.0f*random_Vec3fa()-Vec3fa(1.0f);
              rays[j] = makeRay(pos+org,dir); 
            } else {
              valid[j] = false;
              rays[j] = invalid_ray;
            }
          }
          IntersectWithMode(imode,ivariant,scene,rays,M);
          for (size_t j=0; j<M; j++) {
            if (valid[j]) continue;
            numFailures += neq_ray_special(rays[j],invalid_ray);
          }
        }
      }
      AssertNoError(device);
      return (VerifyApplication::TestReturnValue) (numFailures == 0);
    }
  };

  struct WatertightTest : public VerifyApplication::IntersectTest
  {
    ALIGNED_STRUCT_(16);
    SceneFlags sflags;
    std::string model;
    Vec3fa pos;
    static const size_t N = 10;
    static const size_t maxStreamSize = 100;
    
    WatertightTest (std::string name, int isa, SceneFlags sflags, IntersectMode imode, std::string model, const Vec3fa& pos)
      : VerifyApplication::IntersectTest(name,isa,imode,VARIANT_INTERSECT,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), model(model), pos(pos) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      avector<Vec3fa> motion_vector;
      motion_vector.push_back(Vec3fa(0.0f));
      motion_vector.push_back(Vec3fa(0.0f));

      VerifyScene scene(device,sflags);
      size_t size = state->intensity < 1.0f ? 50 : 500;
      if      (model == "sphere.triangles") scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTriangleSphere(pos,2.0f,size));
      else if (model == "sphere.quads"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadSphere    (pos,2.0f,size));
      else if (model == "sphere.grids"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridSphere    (pos,2.0f,size));
      else if (model == "sphere.subdiv"   ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createSubdivSphere  (pos,2.0f,4,64));
      else if (model == "plane.triangles" ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTrianglePlane (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size));
      else if (model == "plane.quads"     ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadPlane     (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size));
      else if (model == "plane.grids"     ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridPlane     (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size));
      else if (model == "plane.subdiv"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createSubdivPlane   (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size,2));
      else if (model == "sphere.triangles_mb") scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTriangleSphere(pos,2.0f,size)->set_motion_vector(motion_vector));
      else if (model == "sphere.quads_mb"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadSphere    (pos,2.0f,size)->set_motion_vector(motion_vector));
      else if (model == "sphere.grids_mb"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridSphere    (pos,2.0f,size)->set_motion_vector(motion_vector));
      else if (model == "plane.triangles_mb" ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTrianglePlane (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size)->set_motion_vector(motion_vector));
      else if (model == "plane.quads_mb"     ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadPlane     (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size)->set_motion_vector(motion_vector));
      else if (model == "plane.grids_mb"     ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridPlane     (Vec3fa(pos.x,-6.0f,-6.0f),Vec3fa(0.0f,0.0f,12.0f),Vec3fa(0.0f,12.0f,0.0f),size,size)->set_motion_vector(motion_vector));
      else throw std::runtime_error("unsupported mode "+model); 
      bool plane = model.compare(0,5,"plane") == 0;
      rtcCommitScene (scene);
      AssertNoError(device);
      
      size_t numTests = 0;
      size_t numFailures = 0;
      for (auto ivariant : state->intersectVariants)
      for (size_t i=0; i<size_t(N*state->intensity); i++) 
      {
        for (unsigned int M=1; M<maxStreamSize; M++)
        {
          __aligned(16) RTCRayHit rays[maxStreamSize];
          for (size_t j=0; j<M; j++) 
          {
            if (plane) {
              Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f); dir.x = 1.0f;
              rays[j] = makeRay(Vec3fa(pos.x-3.0f,0.0f,0.0f),dir); 
            } else {
              Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
              Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
              rays[j] = makeRay(pos+org,dir); 
            }
          }
          IntersectWithMode(imode,ivariant,scene,rays,M);
          for (unsigned int j=0; j<M; j++) {
            numTests++;
            if (ivariant & VARIANT_INTERSECT)
              numFailures += rays[j].hit.geomID == RTC_INVALID_GEOMETRY_ID;
            else
              numFailures += rays[j].ray.tfar != float(neg_inf);
          }
        }
      }
      AssertNoError(device);

      double failRate = double(numFailures) / double(max(size_t(1),numTests));
      bool failed = failRate > 0.00002;
      if (!silent) { printf(" (%f%%)", 100.0f*failRate); fflush(stdout); }
      return (VerifyApplication::TestReturnValue)(!failed);
    }
  };

  struct SmallTriangleHitTest : public VerifyApplication::IntersectTest
  {
    ALIGNED_STRUCT_(16);
    SceneFlags sflags;
    Vec3fa pos;
    float radius;
    static const size_t N = 10000;
    static const size_t maxStreamSize = 100;
    
    SmallTriangleHitTest (std::string name, int isa, SceneFlags sflags, IntersectMode imode, const Vec3fa& pos, const float radius)
      : VerifyApplication::IntersectTest(name,isa,imode,VARIANT_INTERSECT,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), pos(pos), radius(radius) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      VerifyScene scene(device,sflags);
      LinearSpace3fa space(Vec3fa(1.0f,0.0f,0.0f),Vec3fa(0.0f,1.0f,0.0f),Vec3fa(0.0f,0.0f,1.0f));
      space *= LinearSpace3fa::rotate(Vec3fa(4.0f,7.0f,-1.0f),4.34f);
      const Vec3fa dx = 100.0f*normalize(space.vx);
      const Vec3fa dy = 100.0f*normalize(space.vy);
      const Vec3fa p = pos-0.5f*(dx+dy);
      Ref<SceneGraph::TriangleMeshNode> plane = SceneGraph::createTrianglePlane (p,dx,dy,100,100).dynamicCast<SceneGraph::TriangleMeshNode>();
      scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,plane.dynamicCast<SceneGraph::Node>());
      rtcCommitScene (scene);
      AssertNoError(device);
      
      size_t numTests = 0;
      size_t numFailures = 0;
      //for (auto ivariant : state->intersectVariants)
      IntersectVariant ivariant = VARIANT_INTERSECT_INCOHERENT;
      size_t numRays = size_t(N*state->intensity);
      for (size_t i=0; i<numRays; i+=maxStreamSize) 
      {
        unsigned int M = (unsigned int)min(maxStreamSize,numRays-i);
        unsigned int primIDs[maxStreamSize];
        __aligned(16) RTCRayHit rays[maxStreamSize];
        for (size_t j=0; j<M; j++) 
        {
          Vec3fa org = pos + radius*(2.0f*random_Vec3fa() - Vec3fa(1.0f));
          unsigned int primID = random_int() % plane->triangles.size();
          Vec3fa v0 = plane->positions[0][plane->triangles[primID].v0];
          Vec3fa v1 = plane->positions[0][plane->triangles[primID].v1];
          Vec3fa v2 = plane->positions[0][plane->triangles[primID].v2];
          Vec3fa c = (v0+v1+v2)/3.0f;
          primIDs[j] = primID;
          rays[j] = makeRay(org,c-org); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,M);
        for (size_t j=0; j<M; j++) {
          Vec3fa dir(rays[j].ray.dir_x,rays[j].ray.dir_y,rays[j].ray.dir_z);
          //if (abs(dot(normalize(dir),space.vz)) < 0.9f) continue;
          numTests++;
          numFailures += rays[j].hit.primID != primIDs[j];
        }
      }
      AssertNoError(device);

      double failRate = double(numFailures) / double(max(size_t(1),numTests));
      bool failed = failRate > 0.00002;
      if (!silent) { printf(" (%f%%)", 100.0f*failRate); fflush(stdout); }
      return (VerifyApplication::TestReturnValue)(!failed);
    }
  };

  struct RayAlignmentTest : public VerifyApplication::IntersectTest
  {
    ALIGNED_STRUCT_(16);
    SceneFlags sflags;
    std::string model;
    static const size_t N = 10;
    static const size_t maxStreamSize = 100;
    
    RayAlignmentTest (std::string name, int isa, SceneFlags sflags, IntersectMode imode, std::string model)
      : VerifyApplication::IntersectTest(name,isa,imode,VARIANT_INTERSECT,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), model(model) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      VerifyScene scene(device,sflags);
      if      (model == "sphere.triangles") scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createTriangleSphere(zero,2.0f,50));
      else if (model == "sphere.quads"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createQuadSphere    (zero,2.0f,50));
      else if (model == "sphere.grids"    ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createGridSphere    (zero,2.0f,50));
      else if (model == "sphere.subdiv"   ) scene.addGeometry(RTC_BUILD_QUALITY_MEDIUM,SceneGraph::createSubdivSphere  (zero,2.0f,4,4));
      // FIXME: test more geometry types
      rtcCommitScene (scene);
      AssertNoError(device);
      
      for (auto ivariant : state->intersectVariants)
      for (size_t i=0; i<size_t(N*state->intensity); i++) 
      {
        for (unsigned int M=1; M<maxStreamSize; M++)
        {
          size_t alignment = alignment_of(imode);
          __aligned(64) char data[maxStreamSize*sizeof(RTCRayHit)]; 
          RTCRayHit* rays = (RTCRayHit*) &data[alignment];
          for (size_t j=0; j<M; j++) 
          {
            Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
            Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
            rays[j] = makeRay(org,dir); 
          }
          IntersectWithMode(imode,ivariant,scene,rays,M);
        }
      }
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };
  
  struct NaNTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;
    
    NaNTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality)  {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      bool ok = false;
      for (size_t i=0; i<10 && !ok; i++)
      {
        const size_t numRays = 1000;
        RTCRayHit rays[numRays];
        VerifyScene scene(device,sflags);
        scene.addSphere(sampler,quality,zero,2.0f,100);
        scene.addHair  (sampler,quality,zero,1.0f,1.0f,100);
        rtcCommitScene (scene);
        
        double c0 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c1 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org+Vec3fa(nan),dir); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c2 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org+Vec3fa(nan),dir+Vec3fa(nan)); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c3 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir,nan,nan); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c4 = getSeconds();
        double d1 = c1-c0;
        double d2 = c2-c1;
        double d3 = c3-c2;
        double d4 = c4-c3;
        AssertNoError(device);        
        
        ok = (d2 < 2.5*d1) && (d3 < 2.5*d1) && (d4 < 2.5*d1);
        double f = max(d2/d1,d3/d1,d4/d1);
        if (!silent) { printf(" (%3.2fx)",f); fflush(stdout); }
      }
      return (VerifyApplication::TestReturnValue) ok;
    }
  };
    
  struct InfTest : public VerifyApplication::IntersectTest
  {
    SceneFlags sflags;
    RTCBuildQuality quality;
    
    InfTest (std::string name, int isa, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant)
      : VerifyApplication::IntersectTest(name,isa,imode,ivariant,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), quality(quality) {}
   
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      if (!supportsIntersectMode(device,imode))
        return VerifyApplication::SKIPPED;

      const size_t numRays = 1000;
      RTCRayHit rays[numRays];
      VerifyScene scene(device,sflags);
      scene.addSphere(sampler,quality,zero,2.0f,100);
      scene.addHair  (sampler,quality,zero,1.0f,1.0f,100);
      rtcCommitScene (scene);
      AssertNoError(device);

      bool ok = false;
      for (size_t i=0; i<10 && !ok; i++)
      {
        double c0 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c1 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org+Vec3fa(inf),dir); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c2 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir+Vec3fa(inf)); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c3 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org+Vec3fa(inf),dir+Vec3fa(inf)); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c4 = getSeconds();
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*random_Vec3fa() - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir,-0.0f,inf); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
        
        double c5 = getSeconds();      
        double d1 = c1-c0;
        double d2 = c2-c1;
        double d3 = c3-c2;
        double d4 = c4-c3;
        double d5 = c5-c4;
        AssertNoError(device);
        
        ok = (d2 < 2.5*d1) && (d3 < 2.5*d1) && (d4 < 2.5*d1) && (d5 < 2.5*d1);
        double f = max(d2/d1,d3/d1,d4/d1,d5/d1);
        if (!silent) { printf(" (%3.2fx)",f); fflush(stdout); }
      }
      return (VerifyApplication::TestReturnValue) ok;
    }
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  
  struct PointQueryAPICallsTest : public VerifyApplication::Test
  {
    SceneFlags sflags; 

    PointQueryAPICallsTest (std::string name, int isa, SceneFlags sflags)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags) {}

    VerifyApplication::TestReturnValue run(VerifyApplication *state, bool silent)
    {
      // This test assures that the expected internal point query calls are made
      // for supported primitive/geometry types
      
      std::string cfg = state->rtcore + ",isa=" + stringOfISA(isa);

      auto queryFunc = [](RTCPointQueryFunctionArguments* args) -> bool
      {
        assert(args->userPtr);
        //printf("query callback called for geomID %u and primID %u\n", args->geomID, args->primID);
        uint32_t *numCalls = (uint32_t*)args->userPtr;
        (*numCalls)++;
        return false;
      };
      RTCPointQuery query;
      query.x = query.y = query.z = query.time = 0.f;
      query.radius = inf;

      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr, rtcGetDeviceError(device));
      
      // triangle mesh
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        Vec3f *vertices = (Vec3f *)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Vec3f), 3);
        Triangle *triangles = (Triangle *)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, sizeof(Triangle), 1);
        vertices[0] = Vec3f(-1.0f, 0.0f, -1.0f);
        vertices[1] = Vec3f(+1.0f, 0.0f, -1.0f);
        vertices[2] = Vec3f(+0.0f, 0.0f, +1.0f);
        triangles[0] = Triangle(0, 1, 2);

        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 1)
          return VerifyApplication::FAILED;
        AssertNoError(device);
      }
      
      // flat linear curve
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_FLAT_LINEAR_CURVE);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        Vec4f *vertices = (Vec4f *)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, sizeof(Vec4f), 3);
        uint32_t *indices = (uint32_t*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, sizeof(uint32_t), 2);
        vertices[0] = Vec4f(-1.0f, 0.0f, 0.0f, 0.1f);
        vertices[1] = Vec4f(+0.0f, 0.0f, 0.0f, 0.1f);
        vertices[2] = Vec4f(+1.0f, 0.0f, 0.0f, 0.1f);
        indices[0] = 0;
        indices[1] = 1;

        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 0)
        {
          return VerifyApplication::FAILED;
        }
        AssertNoError(device);
      }
      
      // flat bezier curve
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_FLAT_BEZIER_CURVE);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        Vec4f *vertices = (Vec4f *)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, sizeof(Vec4f), 4);
        uint32_t *indices = (uint32_t*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, sizeof(uint32_t), 1);
        vertices[0] = Vec4f(-1.0f, 0.0f, 0.0f, 0.1f);
        vertices[1] = Vec4f(-1.0f, 0.0f, 1.0f, 0.1f);
        vertices[2] = Vec4f(+1.0f, 0.0f, 1.0f, 0.1f);
        vertices[3] = Vec4f(+1.0f, 0.0f, 0.0f, 0.1f);
        indices[0] = 0;

        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 0)
        {
          return VerifyApplication::FAILED;
        }
        AssertNoError(device);
      }
      
      // grid geometry
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_GRID);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        RTCGrid* grid = (RTCGrid*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_GRID, 0, RTC_FORMAT_GRID, sizeof(RTCGrid), 2);
        grid[0].startVertexID = 0;
        grid[0].stride        = 3;
        grid[0].width         = 3;
        grid[0].height        = 3;
        grid[1].startVertexID = 9;
        grid[1].stride        = 2;
        grid[1].width         = 2;
        grid[1].height        = 2;

        /* set vertices */
        Vec3f* vertices = (Vec3f*) rtcSetNewGeometryBuffer(geom,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,sizeof(Vec3f),13);
        for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i)
        {
          vertices[j * 3 + i] = Vec3f(float(i), (j == 1) ? 0.5f : 1.f, 2.f-float(j));
        }
        vertices[ 9] = Vec3f(0.f, 2.f, 1.f);
        vertices[10] = Vec3f(1.f, 2.f, 1.f);
        vertices[11] = Vec3f(1.f, 2.f, 0.f);
        vertices[12] = Vec3f(0.f, 2.f, 0.f);

        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 2)
        {
          return VerifyApplication::FAILED;
        }
        AssertNoError(device);
      }

      // user geometry
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        auto boundsFunc = [](const struct RTCBoundsFunctionArguments* args)
        {
          const Vec4f* spheres = (const Vec4f*) args->geometryUserPtr;
          RTCBounds* bounds_o = args->bounds_o;
          const Vec4f& sphere = spheres[args->primID];
          bounds_o->lower_x = sphere.x-sphere.w;
          bounds_o->lower_y = sphere.y-sphere.w;
          bounds_o->lower_z = sphere.z-sphere.w;
          bounds_o->upper_x = sphere.x+sphere.w;
          bounds_o->upper_y = sphere.y+sphere.w;
          bounds_o->upper_z = sphere.z+sphere.w;
        };
        auto intersectFunc = [](const RTCIntersectFunctionNArguments* args) {};
        auto occludedFunc  = [](const RTCOccludedFunctionNArguments* args)  {};
        
        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        Vec4f* spheres = (Vec4f*) alignedMalloc(2*sizeof(Vec4f),16);
        spheres[0] = Vec4f(0.f, 0.f, 0.f, 1.f);
        spheres[1] = Vec4f(2.f, 0.f, 0.f, 1.f);
        rtcSetGeometryUserPrimitiveCount(geom, 2);
        rtcSetGeometryUserData(geom, spheres);
        rtcSetGeometryBoundsFunction(geom, boundsFunc, nullptr);
        rtcSetGeometryIntersectFunction(geom, intersectFunc);
        rtcSetGeometryOccludedFunction (geom, occludedFunc);
        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 2)
        {
          return VerifyApplication::FAILED;
        }
        AssertNoError(device);
        alignedFree(spheres);
      }
      
      // point geometry
      if (1) {
        RTCSceneRef scene = rtcNewScene(device);
        rtcSetSceneFlags(scene, sflags.sflags);
        rtcSetSceneBuildQuality(scene, sflags.qflags);

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
        rtcSetGeometryBuildQuality(geom, sflags.qflags);

        Vec4f* vertices = (Vec4f*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, sizeof(Vec4f), 10);
        for (int i = 0; i < 10; ++i)
          vertices[i] = Vec4f((float)i, 0.f, 0.f, 0.1f);

        rtcCommitGeometry(geom);
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
        rtcCommitScene(scene);
        AssertNoError(device);

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        uint32_t numCalls = 0;
        rtcPointQuery(scene, &query, &context, queryFunc, (void*)&numCalls);
        if (numCalls != 0)
        {
          return VerifyApplication::FAILED;
        }
        AssertNoError(device);
      }

      return VerifyApplication::PASSED;
    }
  };
  
  struct PointQueryTest : public VerifyApplication::Test
  {
    SceneFlags sflags; 
    std::string tri_accel;

    PointQueryTest (std::string name, int isa, SceneFlags sflags, std::string tri_accel = "")
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), tri_accel(tri_accel) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa) + ((tri_accel != "") ? ",tri_accel="+tri_accel : "");
      //printf("run test %s\n", cfg.c_str());
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
     
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);
      
      RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom,sflags.qflags);

      Vec3f* vertices = (Vec3f*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, sizeof(Vec3f), 3*32);
      Triangle* triangles = (Triangle*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX , 0, RTC_FORMAT_UINT3, sizeof(Triangle), 32);
      for (int i = 0; i < 32; ++i) {
        float xi = random_float();
        vertices[3*i+0] = Vec3f(0.0f,          0.0f,          (float)i);
        vertices[3*i+1] = Vec3f(1.0f + 5.f*xi, 0.0f,          (float)i);
        vertices[3*i+2] = Vec3f(0.0f,          1.0f + 5.f*xi, (float)i);
        triangles[i] = Triangle(3*i+0, 3*i+1, 3*i+2);
      };

      rtcCommitGeometry(geom);
      rtcAttachGeometry(scene,geom);
      rtcReleaseGeometry(geom);
      rtcCommitScene (scene);
      AssertNoError(device);

      struct UserData
      {
        Vec3f* vertices;
        Triangle* triangles;
        Vec3f result;
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
      };

      for (int i = 0; i < 64; ++i)
      {
        RTCPointQuery query;
        query.x = 0.25f;
        query.y = 0.75f;
        query.z = -0.25f + i * 0.5f;
        query.time = 0.f;
        query.radius = inf;

        UserData data;
        data.vertices  = vertices;
        data.triangles = triangles;

        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query, &context, [](RTCPointQueryFunctionArguments* args) -> bool
        {
          UserData* data = (UserData*)args->userPtr;
          // get triangle info
          Triangle const& t = data->triangles[args->primID];
          Vec3f const& v0 = data->vertices[t.v0];
          Vec3f const& v1 = data->vertices[t.v1];
          Vec3f const& v2 = data->vertices[t.v2];
          
          // determine closest point on triangle
          const Vec3f q(args->query->x, args->query->y, args->query->z);
          const Vec3f p = closestPointTriangle(q, v0, v1, v2);
          const float d = distance(q, p);

          if (d < args->query->radius) {
            args->query->radius = d;
            data->result = p;
            data->primID = args->primID;
            return true;
          }
          return false; 
        }, &data);

        if ((int)data.primID != i/2) return VerifyApplication::FAILED;
        if (abs(data.result.x- 0.25f) > 1e-4f)        return VerifyApplication::FAILED;
        if (abs(data.result.y- 0.75f) > 1e-4f)        return VerifyApplication::FAILED;
        if (abs(data.result.z- (float)(i/2)) > 1e-4f) return VerifyApplication::FAILED;
        AssertNoError(device);
      }

      return VerifyApplication::PASSED;
    }
  };

  struct PointQueryMotionBlurTest : public VerifyApplication::Test
  {
    SceneFlags sflags; 
    std::string tri_accel;

    PointQueryMotionBlurTest (std::string name, int isa, SceneFlags sflags, std::string tri_accel = "")
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), sflags(sflags), tri_accel(tri_accel) {}

    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa) + (tri_accel != "" ? ",tri_accel="+tri_accel : "");
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
     
      Vec3f vertices_t0[4] = { 
        Vec3f(-1.f, -1.f, -1.f), 
        Vec3f( 1.f, -1.f, -1.f), 
        Vec3f( 0.f,  1.f, -1.f), 
        Vec3f(0.f) // 16 byte padding
      };
      Vec3f vertices_t1[4] = { 
        Vec3f(-1.f, -1.f, 1.f), 
        Vec3f( 1.f, -1.f, 1.f), 
        Vec3f( 0.f,  1.f, 1.f), 
        Vec3f(0.f) // 16 byte padding
      };

      // duplicate triangle to make sure the bvh is not only a leaf node
      Triangle triangles[64];
      for (int i = 0; i < 64; ++i)
        triangles[i] = Triangle(0, 1, 2);

      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene,sflags.sflags);
      rtcSetSceneBuildQuality(scene,sflags.qflags);
      
      RTCGeometry geom = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
      rtcSetGeometryBuildQuality(geom,sflags.qflags);
      rtcSetGeometryTimeStepCount(geom,2);
      rtcSetGeometryPointQueryFunction(geom, [](RTCPointQueryFunctionArguments* args) -> bool
      {
        // set primID (userPtr) to something != RTC_INVALID_GEOMETRY_ID to signal that 
        // the query overlapped some geometry
        *((unsigned int*)args->userPtr) = 0;
        return true;
      });
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, vertices_t0, 0, sizeof(Vec3f),     3);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 1, RTC_FORMAT_FLOAT3, vertices_t1, 0, sizeof(Vec3f),     3);
      rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX , 0, RTC_FORMAT_UINT3,  triangles,   0, sizeof(Triangle), 64);
      rtcCommitGeometry(geom);
      rtcAttachGeometry(scene,geom);
      rtcReleaseGeometry(geom);
      rtcCommitScene (scene);
      AssertNoError(device);

      RTCPointQuery query0;
      query0.x      =  0.0f;
      query0.y      =  0.0f;
      query0.z      = -1.0f;
      query0.radius =  0.1f;
      
      RTCPointQuery query1;
      query1.x      =  0.0f;
      query1.y      =  0.0f;
      query1.z      =  0.0f;
      query1.radius =  0.1f;
      
      RTCPointQuery query2;
      query2.x      =  0.0f;
      query2.y      =  0.0f;
      query2.z      =  1.0f;
      query2.radius =  0.1f;

      // time t = 0, only query0 should overlap
      query0.time = query1.time = query2.time = 0.f;
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query0, &context, nullptr, &primID);
        if (primID == RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query1, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query2, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }

      // time t = 0.5, only query1 should overlap
      query0.time = query1.time = query2.time = 0.5f;
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query0, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query1, &context, nullptr, &primID);
        if (primID == RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query2, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      
      // time t = 1.0, only query2 should overlap
      query0.time = query1.time = query2.time = 1.f;
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query0, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query1, &context, nullptr, &primID);
        if (primID != RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }
      {
        unsigned int primID = RTC_INVALID_GEOMETRY_ID;
        RTCPointQueryContext context;
        rtcInitPointQueryContext(&context);
        rtcPointQuery(scene, &query2, &context, nullptr, &primID);
        if (primID == RTC_INVALID_GEOMETRY_ID) return VerifyApplication::FAILED;
      }

      return VerifyApplication::PASSED;
    }
  };

  struct GeometryStateTest : public VerifyApplication::Test
  {
    GeometryStateTest (std::string name, int isa)
      : VerifyApplication::Test (name, isa, VerifyApplication::TEST_SHOULD_PASS) {}
    
    VerifyApplication::TestReturnValue run (VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa=" + stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler (nullptr, rtcGetDeviceError(device));
      AssertNoError(device);
      RTCGeometry geom0 = rtcNewGeometry (device, RTC_GEOMETRY_TYPE_TRIANGLE);
      AssertNoError(device);
      using embree::Geometry;
      auto geometry = (Geometry *) geom0;

      // test construction
      if (geometry->state != (unsigned)Geometry::State::MODIFIED) {
        return VerifyApplication::FAILED;
      }

      //test update
      geometry->state = (unsigned)Geometry::State::COMMITTED;
      geometry->update();
      if (geometry->state != (unsigned)Geometry::State::MODIFIED) {
        return VerifyApplication::FAILED;
      }

      //test commit
      geometry->state = (unsigned)Geometry::State::MODIFIED;
      geometry->commit();
      if (geometry->state != (unsigned)Geometry::State::COMMITTED) {
        return VerifyApplication::FAILED;
      }

      // test disable
      geometry->enabled = false;
      geometry->disable ();
      if (geometry->isEnabled ()) {
        return VerifyApplication::FAILED;
      }

      geometry->enabled = true;
      geometry->disable ();
      if (geometry->isEnabled ()) {
        return VerifyApplication::FAILED;
      }

      // test enable
      geometry->enabled = true;
      geometry->enable ();
      if (!geometry->isEnabled ()) {
        return VerifyApplication::FAILED;
      }

      geometry->enabled = false;
      geometry->enable ();
      if (!geometry->isEnabled ()) {
        return VerifyApplication::FAILED;
      }

      rtcReleaseGeometry(geom0);
      AssertNoError(device);

      return VerifyApplication::PASSED;
    }
  };

  struct SceneCheckModifiedGeometryTest : public VerifyApplication::Test
  {
	  struct TestScene : public Scene {

		  __forceinline void setGeomCounter(size_t geomID, unsigned int count) {
			  geometryModCounters_[geomID] = count;
		  }

		  __forceinline unsigned int getGeomCount(size_t geomID) {
			  return geometryModCounters_[geomID];
		  }
			 
		  __forceinline void checkIfModifiedAndSet() {
			  return Scene::checkIfModifiedAndSet();
		  }
	  };

	  SceneCheckModifiedGeometryTest (std::string name, int isa) 
		: VerifyApplication::Test(name, isa, VerifyApplication::TEST_SHOULD_PASS)
	  {}

	  VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
	  {
		  std::string cfg = state->rtcore + ",isa=" + stringOfISA(isa);
		  RTCDeviceRef device = rtcNewDevice(cfg.c_str());
		  errorHandler(nullptr, rtcGetDeviceError(device));

		  RTCSceneRef scene = rtcNewScene(device);
		  AssertNoError(device);

		  RTCGeometry geom0 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		  AssertNoError(device);
		  RTCGeometry geom1 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		  AssertNoError(device);
		  RTCGeometry geom2 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		  AssertNoError(device);
		  RTCGeometry geom3 = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
		  AssertNoError(device);

		  rtcAttachGeometry(scene, geom0);
		  rtcAttachGeometry(scene, geom1);
		  rtcAttachGeometry(scene, geom2);
		  rtcAttachGeometry(scene, geom3);

		  auto scene0 = (TestScene*)scene.scene;
		  auto geometry0 = (Geometry*) geom0;
		  auto geometry1 = (Geometry*) geom1;
		  auto geometry2 = (Geometry*) geom2;
		  auto geometry3 = (Geometry*) geom3;

		  for (size_t geomID = 0; geomID < 4; ++geomID) {
			  scene0->setGeomCounter(geomID, 1);
		  }

		  scene0->setModified();
		  scene0->checkIfModifiedAndSet();
		  if (!scene0->isModified()) {
			  return VerifyApplication::FAILED;
		  }

		  scene0->setModified(false);
		  geometry0->enable();
		  geometry1->enable();
		  geometry2->enable();
		  geometry3->enable();
		  scene0->checkIfModifiedAndSet();
		  if (scene0->isModified()) {
			  return VerifyApplication::FAILED;
		  }

		  geometry2->modCounter_ += 2;
			
		  scene0->checkIfModifiedAndSet();
		  if (!scene0->isModified()) {
			  return VerifyApplication::FAILED;
		  }

		  rtcReleaseGeometry(geom0);
		  AssertNoError(device);
		  rtcReleaseGeometry(geom1);
		  AssertNoError(device);
		  rtcReleaseGeometry(geom2);
		  AssertNoError(device);
		  rtcReleaseGeometry(geom3);
		  AssertNoError(device);

		  return VerifyApplication::PASSED;
	  }
  };

  struct SphereFilterMultiHitTest : public VerifyApplication::Test
  {
    struct IntersectContext
    {
      RTCIntersectContext context;
      void* userRayExt;         
    };
    
    SphereFilterMultiHitTest (std::string name, int isa) 
      : VerifyApplication::Test(name, isa, VerifyApplication::TEST_SHOULD_PASS)
    {}
    
    void createSphere(RTCDevice device, RTCScene scene, float x, float y, float z, float r)
    {
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_SPHERE_POINT);
      
      float* vertices = (float*)rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4 * sizeof(float), 1);
      vertices[0] = x; vertices[1] = y; vertices[2] = z; vertices[3] = r;
      
      rtcCommitGeometry(geom);
      rtcAttachGeometry(scene, geom);
      rtcReleaseGeometry(geom);
    }
    
    static void countHits(const RTCFilterFunctionNArguments* args)
    {
      assert(args->N == 1);
      RTCRay* ray = (RTCRay*) args->ray;
      auto pos = embree::Vec3f(ray->org_x, ray->org_y, ray->org_z) + embree::Vec3f(ray->dir_x, ray->dir_y, ray->dir_z) * ray->tfar;
      static_cast<std::vector<embree::Vec3f>*>(((IntersectContext*)args->context)->userRayExt)->push_back (pos);
      args->valid[0] = 0;
    }
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      RTCDeviceRef device = rtcNewDevice(nullptr);
      RTCSceneRef scene = rtcNewScene(device);
      rtcSetSceneFlags(scene, RTC_SCENE_FLAG_CONTEXT_FILTER_FUNCTION | RTC_SCENE_FLAG_ROBUST);
      
      createSphere(device, scene, 0, 0, 0, 1);
      createSphere(device, scene, 0, 0, 3, 1);
      
      rtcCommitScene(scene);
      
      IntersectContext intersectContext;
      std::vector<embree::Vec3f> hits;
      intersectContext.userRayExt = &hits;
      rtcInitIntersectContext(&(intersectContext.context));
      intersectContext.context.filter = &countHits;
      
      RTCRayHit rayHit;
      rayHit.ray.org_x = 0;
      rayHit.ray.org_y = 0;
      rayHit.ray.org_z = -5;
      rayHit.ray.dir_x = 0;
      rayHit.ray.dir_y = 0;
      rayHit.ray.dir_z = 1;
      rayHit.ray.tnear = 0;
      rayHit.ray.tfar = 100000;
      rayHit.ray.mask = 0u;
      rayHit.ray.flags = 0u;
      rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
      rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
      
      rtcIntersect1(scene, &(intersectContext.context), &rayHit);
      
      bool cullingEnabled = rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_BACKFACE_CULLING_ENABLED);
      
      if (cullingEnabled) {
        if (hits.size() != 2) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[0].z + 1.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[1].z - 1.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
      } else {
        if (hits.size() != 4) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[0].z + 1.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[1].z - 1.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[2].z - 2.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
        if(std::fabs(hits[3].z - 4.f) > 1.e-6) {
          return VerifyApplication::FAILED;
        }
      }
      
      return VerifyApplication::PASSED;
    }
  };
  
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  void shootRandomRays (int hash, std::vector<IntersectMode>& intersectModes, std::vector<IntersectVariant>& intersectVariants, const VerifyScene& scene)
  {
    RandomSampler sampler;
    RandomSampler_init(sampler,hash);

    const size_t numRays = 100;
    for (auto imode : intersectModes)
    {
      for (auto ivariant : intersectVariants)
      {
        if (!has_variant(imode,ivariant)) continue;

        RTCRayHit rays[numRays];
        for (size_t i=0; i<numRays; i++) {
          Vec3fa org = 2.0f*RandomSampler_get3D(sampler) - Vec3fa(1.0f);
          Vec3fa dir = 2.0f*RandomSampler_get3D(sampler) - Vec3fa(1.0f);
          rays[i] = makeRay(org,dir); 
        }
        IntersectWithMode(imode,ivariant,scene,rays,numRays);
      }
    }
  }

  struct RegressionTask
  {
    RegressionTask (VerifyApplication::Test* test, unsigned int sceneIndex, unsigned int sceneCount, unsigned int threadCount, bool cancelBuild)
      : sceneIndex(sceneIndex), sceneCount(sceneCount), test(test), scene(nullptr), numActiveThreads(0), cancelBuild(cancelBuild), errorCounter(0) 
    { 
      barrier.init(threadCount); 
      RandomSampler_init(sampler,0);
    }

    unsigned int sceneIndex;
    unsigned int sceneCount;
    VerifyApplication::Test* test;
    VerifyApplication* state;
    Ref<VerifyScene> scene;
    BarrierSys barrier;
    volatile unsigned int numActiveThreads;
    bool cancelBuild;
    std::atomic<size_t> errorCounter;
    RandomSampler sampler;
  };

  struct ThreadRegressionTask
  {
    ThreadRegressionTask (unsigned int threadIndex, unsigned int threadCount,
                          VerifyApplication* state, RTCDeviceRef& device, std::vector<IntersectMode>& intersectModes, RegressionTask* task)
      : threadIndex(threadIndex), threadCount(threadCount), state(state), device(device), intersectModes(intersectModes), task(task) {}

    unsigned int threadIndex;
    unsigned int threadCount;
    VerifyApplication* state;
    RTCDeviceRef& device;
    std::vector<IntersectMode>& intersectModes;
    RegressionTask* task;
  };

  size_t monitorProgressBreak = 0;
  std::atomic<size_t> monitorProgressInvokations(0);
  std::atomic<size_t> monitorProgressBreakExecuted(0);

  bool monitorProgressFunction(void* ptr, double dn) 
  {
    size_t n = monitorProgressInvokations++;
    if (n == monitorProgressBreak) {
      monitorProgressBreakExecuted++;
      return false;
    }
    return true;
  }

  void rtcore_regression_static_thread(void* ptr)
  {
    ThreadRegressionTask* thread = (ThreadRegressionTask*) ptr;
    RegressionTask* task = thread->task;
    if (thread->threadIndex > 0) 
    {
      for (unsigned int i=0; i<task->sceneCount; i++) 
      {
	task->barrier.wait();
	if (thread->threadIndex < task->numActiveThreads) 
	{
          rtcJoinCommitScene(*task->scene);
	  //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
          if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) {
            task->errorCounter++;
          }
          else {
            shootRandomRays(i,thread->intersectModes,thread->state->intersectVariants,*task->scene);
          }
	}
        task->barrier.wait();
      }
      delete thread; thread = nullptr;
      return;
    }

    if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
    bool hasError = false;

    for (unsigned int i=0; i<task->sceneCount; i++) 
    {
      RandomSampler_init(task->sampler,task->sceneIndex*13565+i*3242);
      if (i%5 == 0) std::cout << "." << std::flush;

      SceneFlags sflags = getSceneFlags(i);
      task->scene = new VerifyScene(thread->device,sflags);
      if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
      if (task->cancelBuild) rtcSetSceneProgressMonitorFunction(*task->scene,monitorProgressFunction,nullptr);
      std::vector<std::unique_ptr<Sphere>> spheres;
      
      for (unsigned int j=0; j<10; j++) 
      {
        Vec3fa pos = 100.0f*RandomSampler_get3D(task->sampler);
	int type = RandomSampler_getInt(task->sampler)%11;
        switch (RandomSampler_getInt(task->sampler)%16) {
        case 0: pos = Vec3fa(nan); break;
        case 1: pos = Vec3fa(inf); break;
        case 2: pos = Vec3fa(1E30f); break;
        default: break;
        };
	size_t numPhi = RandomSampler_getInt(task->sampler)%50;
	if (type == 2) numPhi = RandomSampler_getInt(task->sampler)%10;
        size_t numTriangles = 2*2*numPhi*(numPhi-1);
	numTriangles = RandomSampler_getInt(task->sampler)%(numTriangles+1);
        switch (type) {
        case 0: task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); break;
	case 1: task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); break;
        case 2: task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); break;
	case 3: task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); break;
        case 4: task->scene->addGridSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); break;
	case 5: task->scene->addGridSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); break;
	case 6: task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,4,numTriangles); break;
        case 7: task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,4,numTriangles,task->test->random_motion_vector(1.0f)); break;
	case 8: task->scene->addHair  (task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,1.0f,2.0f,numTriangles); break;
	case 9: task->scene->addHair  (task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,1.0f,2.0f,numTriangles,task->test->random_motion_vector(1.0f)); break; 

        case 10: {
	  std::unique_ptr<Sphere> sphere(new Sphere(pos,2.0f));  
	  task->scene->addUserGeometryEmpty(task->sampler,RTC_BUILD_QUALITY_MEDIUM,sphere.get());
          spheres.push_back(std::move(sphere));
          break;
        }
	}
        //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
        if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) {
          task->errorCounter++;
          hasError = true;
          break;
        }
      }
      
      if (thread->threadCount) {
	task->numActiveThreads = max(unsigned(1),RandomSampler_getInt(task->sampler) % thread->threadCount);
	task->barrier.wait();
        rtcJoinCommitScene(*task->scene);
      } else {
        if (!hasError) {
          rtcCommitScene(*task->scene);
        }
      }
      //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;

      if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) {
        task->errorCounter++;
      }
      else {
        if (!hasError) {
          shootRandomRays(i,thread->intersectModes,thread->state->intersectVariants,*task->scene);
        }
      }

      if (thread->threadCount) 
	task->barrier.wait();

      task->scene = nullptr;
      if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;
    }

    delete thread; thread = nullptr;
    return;
  }

  void rtcore_regression_dynamic_thread(void* ptr)
  {
    ThreadRegressionTask* thread = (ThreadRegressionTask*) ptr;
    RegressionTask* task = thread->task;
    if (thread->threadIndex > 0) 
    {
      for (unsigned int i=0; i<task->sceneCount; i++) 
      {
	task->barrier.wait();
	if (thread->threadIndex < task->numActiveThreads) 
	{
          rtcJoinCommitScene(*task->scene);
	  //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
          if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) {
            task->errorCounter++;
          }
          else {
            shootRandomRays(i,thread->intersectModes,thread->state->intersectVariants,*task->scene);
          }
	}
	task->barrier.wait();
      }
      delete thread; thread = nullptr;
      return;
    }

    RTCBuildQuality build_quality = RTC_BUILD_QUALITY_LOW;
    if (RandomSampler_getInt(task->sampler)%2) build_quality = RTC_BUILD_QUALITY_MEDIUM;
    
    task->scene = new VerifyScene(thread->device,SceneFlags(RTC_SCENE_FLAG_DYNAMIC,build_quality));
    if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
    if (task->cancelBuild) rtcSetSceneProgressMonitorFunction(*task->scene,monitorProgressFunction,nullptr);
    const size_t numSlots = 20;
    const size_t numIterations = 2*numSlots;
    std::pair<int,Ref<SceneGraph::Node>> geom[numSlots];
    int types[numSlots];
    RTCBuildQuality quality[numSlots];
    Sphere spheres[numSlots];
    size_t numVertices[numSlots];
    for (size_t i=0; i<numSlots; i++)  {
      geom[i].first = -1;
      geom[i].second = nullptr;
      types[i] = 0;
      numVertices[i] = 0;
    }

    bool hasError = false;

    for (unsigned int i=0; i<task->sceneCount; i++) 
    {
      srand(task->sceneIndex*23565+i*2242); // FIXME: required?
      if (i%20 == 0) std::cout << "." << std::flush;

      for (unsigned int j=0; j<numIterations; j++) 
      {
        int index = RandomSampler_getInt(task->sampler)%numSlots;
        if (geom[index].first == -1) 
        {
          int type = RandomSampler_getInt(task->sampler)%27;
          
          Vec3fa pos = 100.0f*RandomSampler_get3D(task->sampler);
          switch (RandomSampler_getInt(task->sampler)%16) {
          case 0: pos = Vec3fa(nan); break;
          case 1: pos = Vec3fa(inf); break;
          case 2: pos = Vec3fa(1E30f); break;
          default: break;
          };
          size_t numPhi = RandomSampler_getInt(task->sampler)%100;
	  if (type >= 12 && type <= 17) numPhi = RandomSampler_getInt(task->sampler)%10;
#if defined(__WIN32__)          
          numPhi = RandomSampler_getInt(task->sampler) % 4;
#endif

          size_t numTriangles = 2*2*numPhi*(numPhi-1);
          numTriangles = RandomSampler_getInt(task->sampler)%(numTriangles+1);
          types[index] = type;
          numVertices[index] = 2*numPhi*(numPhi+1);
          switch (type) {
          case 0: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 1: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 2: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 3: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 4: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 5: geom[index] = task->scene->addSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 6: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 7: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 8: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 21: geom[index] = task->scene->addGridSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 22: geom[index] = task->scene->addGridSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 23: geom[index] = task->scene->addGridSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,numTriangles); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 9: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 10: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 11: geom[index] = task->scene->addQuadSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 12: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,4,numTriangles); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
	  case 13: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,4,numTriangles); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
	  case 14: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,4,numTriangles); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 15: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,2.0f,numPhi,4,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
	  case 16: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_REFIT,pos,2.0f,numPhi,4,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
	  case 17: geom[index] = task->scene->addSubdivSphere(task->sampler,RTC_BUILD_QUALITY_LOW,pos,2.0f,numPhi,4,numTriangles,task->test->random_motion_vector(1.0f)); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 18: spheres[index] = Sphere(pos,2.0f); geom[index] = task->scene->addUserGeometryEmpty(task->sampler,RTC_BUILD_QUALITY_MEDIUM,&spheres[index]); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 19: spheres[index] = Sphere(pos,2.0f); geom[index] = task->scene->addUserGeometryEmpty(task->sampler,RTC_BUILD_QUALITY_REFIT,&spheres[index]); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 20: spheres[index] = Sphere(pos,2.0f); geom[index] = task->scene->addUserGeometryEmpty(task->sampler,RTC_BUILD_QUALITY_LOW,&spheres[index]); quality[index] = RTC_BUILD_QUALITY_LOW; break;

          case 24: geom[index] = task->scene->addHair  (task->sampler,RTC_BUILD_QUALITY_MEDIUM,pos,1.0f,2.0f,numTriangles); quality[index] = RTC_BUILD_QUALITY_MEDIUM; break;
          case 25: geom[index] = task->scene->addHair  (task->sampler,RTC_BUILD_QUALITY_REFIT,pos,1.0f,2.0f,numTriangles); quality[index] = RTC_BUILD_QUALITY_REFIT; break;
          case 26: geom[index] = task->scene->addHair  (task->sampler,RTC_BUILD_QUALITY_LOW,pos,1.0f,2.0f,numTriangles); quality[index] = RTC_BUILD_QUALITY_LOW; break;
          }; 
	  //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
          if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) {
            task->errorCounter++;
            hasError = true;
            break;
          }
        }
        else 
        {
          switch (types[index]) {
          case 18:
          case 19:
          case 20:
          {
            rtcDetachGeometry(*task->scene,geom[index].first);     
	    if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
            geom[index].first = -1; 
            geom[index].second = nullptr; 
            break;
          }
          case 0:
          case 1: 
          case 2:
          case 3:
          case 4: 
          case 5:
          case 6:
	  case 7: 
          case 8:
          case 9:
          case 10:
          case 11:
          case 12:
          case 13:
          case 14:
          case 15:
          case 16:
          case 17:
          case 21:
          case 22:
          case 23:
          case 24:
          case 25:
          case 26:
          {
            int op = RandomSampler_getInt(task->sampler)%4;
            switch (op) {
            case 0: {
              rtcDetachGeometry(*task->scene,geom[index].first);     
	      if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;
              geom[index].first = -1; 
              geom[index].second = nullptr; 
              break;
            }
            case 1: {

              switch (types[index])
              {
              case 24: case 25: case 26: break; // does not work for hair for some reason
              default:
                RTCGeometry hgeom = rtcGetGeometry(*task->scene,geom[index].first);
                Vec3fa* vertices = (Vec3fa*) rtcGetGeometryBufferData(hgeom, RTC_BUFFER_TYPE_VERTEX, 0);
                if (vertices) { 
                  for (size_t i=0; i<numVertices[index]; i++) vertices[i] += Vec3fa(0.1f);
                }
                rtcUpdateGeometryBuffer(hgeom,RTC_BUFFER_TYPE_VERTEX, 0);
                
                switch (types[index])
                {
                case 4: case 5: case 10: case 11:
                  RTCGeometry hgeom = rtcGetGeometry(*task->scene, geom[index].first);
                  Vec3fa* vertices = (Vec3fa*)rtcGetGeometryBufferData(hgeom, RTC_BUFFER_TYPE_VERTEX, 1);
                  if (vertices) {
                    for (size_t i = 0; i < numVertices[index]; i++) vertices[i] += Vec3fa(0.1f);
                  }
                  rtcUpdateGeometryBuffer(hgeom,RTC_BUFFER_TYPE_VERTEX, 1);
                }
                rtcCommitGeometry(hgeom);
                break;
              }
              break;
            }
            case 2: {
              switch (types[index])
              {
              case 2:
              case 5:
              case 8:
              case 11:
              case 14:
              case 23:
                RTCGeometry hgeom = rtcGetGeometry(*task->scene, geom[index].first);
                task->scene->resizeRandomly(geom[index],task->sampler);
                rtcCommitGeometry(hgeom);
                break;
              }
            }
            case 3: {
              RTCGeometry hgeom = rtcGetGeometry(*task->scene, geom[index].first);
              RTCBuildQuality q = (RTCBuildQuality) (RandomSampler_getInt(task->sampler)%4);
              rtcSetGeometryBuildQuality(hgeom,q);
              quality[index] = q;
              rtcCommitGeometry(hgeom);
            }
            }
            break;
          }
          }
        }       
      }

      if (thread->threadCount) {
	task->numActiveThreads = max(unsigned(1),RandomSampler_getInt(task->sampler) % thread->threadCount);
	task->barrier.wait();
        rtcJoinCommitScene(*task->scene);
      } else {
        if (!hasError) 
          rtcCommitScene(*task->scene);
      }
      //if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;

      if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE)
        task->errorCounter++;
      else
        if (!hasError)
          shootRandomRays(i,thread->intersectModes,thread->state->intersectVariants,*task->scene);

      if (thread->threadCount) 
	task->barrier.wait();
    }

    task->scene = nullptr;
    if (rtcGetDeviceError(thread->device) != RTC_ERROR_NONE) task->errorCounter++;;

    delete thread; thread = nullptr;
    return;
  }

  struct IntensiveRegressionTest : public VerifyApplication::Test
  {
    thread_func func;
    int mode;
    float intensity;
    std::vector<IntersectMode> intersectModes;
    std::vector<thread_t> threads;
    
    IntensiveRegressionTest (std::string name, int isa, thread_func func, int mode, float intensity)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), func(func), mode(mode), intensity(intensity) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));

      /* only test supported intersect modes */
      intersectModes.push_back(MODE_INTERSECT1);
      intersectModes.push_back(MODE_INTERSECT4);
      intersectModes.push_back(MODE_INTERSECT8);
      intersectModes.push_back(MODE_INTERSECT16);
      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_RAY_STREAM_SUPPORTED)) {
        intersectModes.push_back(MODE_INTERSECT1M);
        intersectModes.push_back(MODE_INTERSECT1Mp);
        intersectModes.push_back(MODE_INTERSECTNM1);
        intersectModes.push_back(MODE_INTERSECTNM3);
        intersectModes.push_back(MODE_INTERSECTNM4);
        intersectModes.push_back(MODE_INTERSECTNM8);
        intersectModes.push_back(MODE_INTERSECTNM16);
        intersectModes.push_back(MODE_INTERSECTNp);
      }

      size_t errorCounter = 0;
      unsigned int sceneIndex = 0;
      while (sceneIndex < size_t(intensity*state->intensity)) 
      {
        if (mode)
        {
          unsigned numThreads = getNumberOfLogicalThreads();
          
          std::vector<RegressionTask*> tasks;
          while (numThreads) 
          {
            unsigned int N = max(unsigned(1),random_int()%numThreads); numThreads -= N;
            RegressionTask* task = new RegressionTask(this,sceneIndex++,5,N,false);
            tasks.push_back(task);
            
            for (unsigned int i=0; i<N; i++) 
              threads.push_back(createThread(func,new ThreadRegressionTask(i,N,state,device,intersectModes,task),DEFAULT_STACK_SIZE,numThreads+i));
          }
          
          for (size_t i=0; i<threads.size(); i++)
            join(threads[i]);
          for (size_t i=0; i<tasks.size(); i++) {
            errorCounter += tasks[i]->errorCounter;
            delete tasks[i];
          }
          
          threads.clear();
        }
        else
        {
          RegressionTask task(this,sceneIndex++,5,0,false);
          func(new ThreadRegressionTask(0,0,state,device,intersectModes,&task));
        }	
      }
      return (VerifyApplication::TestReturnValue) (errorCounter == 0);
    }
  };

  size_t monitorMemoryBreak = 0;
  std::atomic<size_t> monitorMemoryBytesUsed(0);
  std::atomic<size_t> monitorMemoryInvokations(0);
  std::atomic<size_t> monitorMemoryBreakExecuted(0);

  bool monitorMemoryFunction(void* userPtr, ssize_t bytes, bool post) 
  {
    monitorMemoryBytesUsed += bytes;
    if (bytes > 0) {
      size_t n = monitorMemoryInvokations++;
      if (n == monitorMemoryBreak) {
        monitorMemoryBreakExecuted++;
        if (!post) monitorMemoryBytesUsed -= bytes;
        return false;
      }
    }
    return true;
  }

  struct MemoryMonitorTest : public VerifyApplication::Test
  {
    thread_func func;
    float intensity;
    std::vector<IntersectMode> intersectModes;
    
    MemoryMonitorTest (std::string name, int isa, thread_func func, float intensity)
      : VerifyApplication::Test(name,isa,VerifyApplication::TEST_SHOULD_PASS), func(func), intensity(intensity) {}
    
    VerifyApplication::TestReturnValue run(VerifyApplication* state, bool silent)
    {
      std::string cfg = state->rtcore + ",isa="+stringOfISA(isa);
      RTCDeviceRef device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      
      /* only test supported intersect modes */
      intersectModes.push_back(MODE_INTERSECT1);
      intersectModes.push_back(MODE_INTERSECT4);
      intersectModes.push_back(MODE_INTERSECT8);
      intersectModes.push_back(MODE_INTERSECT16);
      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_RAY_STREAM_SUPPORTED)) {
        intersectModes.push_back(MODE_INTERSECT1M);
        intersectModes.push_back(MODE_INTERSECT1Mp);
        intersectModes.push_back(MODE_INTERSECTNM1);
        intersectModes.push_back(MODE_INTERSECTNM3);
        intersectModes.push_back(MODE_INTERSECTNM4);
        intersectModes.push_back(MODE_INTERSECTNM8);
        intersectModes.push_back(MODE_INTERSECTNM16);
        intersectModes.push_back(MODE_INTERSECTNp);
      }
      
      rtcSetDeviceMemoryMonitorFunction(device,monitorMemoryFunction,nullptr);
      
      unsigned int sceneIndex = 0;
      while (sceneIndex < size_t(intensity*state->intensity)) 
      {
        monitorMemoryBreak = std::numeric_limits<size_t>::max();
        monitorMemoryBytesUsed = 0;
        monitorMemoryInvokations = 0;
        monitorMemoryBreakExecuted = 0;
        monitorProgressBreak = std::numeric_limits<size_t>::max();
        monitorProgressInvokations = 0;
        monitorProgressBreakExecuted = 0;
        RegressionTask task1(this,sceneIndex,1,0,true);
        func(new ThreadRegressionTask(0,0,state,device,intersectModes,&task1));
        if (monitorMemoryBytesUsed) 
          return VerifyApplication::FAILED;

        monitorMemoryBreak = size_t(float(monitorMemoryInvokations) * random_float());
        monitorMemoryBytesUsed = 0;
        monitorMemoryInvokations = 0;
        monitorMemoryBreakExecuted = 0;
        monitorProgressBreak = size_t(float(monitorProgressInvokations) * 2.0f * random_float());
        monitorProgressInvokations = 0;
        monitorProgressBreakExecuted = 0;
        RegressionTask task2(this,sceneIndex,1,0,true);
        func(new ThreadRegressionTask(0,0,state,device,intersectModes,&task2));
        if (monitorMemoryBytesUsed) 
          return VerifyApplication::FAILED;
        if (monitorMemoryBreakExecuted && task2.errorCounter != 1) 
          return VerifyApplication::FAILED;
        if (monitorProgressBreakExecuted && task2.errorCounter != 1) 
          return VerifyApplication::FAILED;
        if (!monitorMemoryBreakExecuted && !monitorProgressBreakExecuted && task2.errorCounter != 0) 
          return VerifyApplication::FAILED;
        sceneIndex++;
      }
      rtcSetDeviceMemoryMonitorFunction(device,nullptr,nullptr);
      AssertNoError(device);
      return VerifyApplication::PASSED;
    }
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  struct SimpleBenchmark : public VerifyApplication::Benchmark
  {
    SimpleBenchmark (std::string name, int isa)
      : VerifyApplication::Benchmark(name,isa,"1/s",true,10) {}
    
    float benchmark(VerifyApplication* state)
    {
      double t0 = getSeconds();
      for (volatile size_t i=0; i<10000000; i++);
      double t1 = getSeconds();
      return 1.0f/float(t1-t0);
    }
  };
  
  struct ParallelIntersectBenchmark : public VerifyApplication::Benchmark
  {
    unsigned int N, dN;
    
    ParallelIntersectBenchmark (std::string name, int isa, unsigned int N, unsigned int dN)
      : VerifyApplication::Benchmark(name,isa,"Mrps",true,10), N(N), dN(dN) {}

    bool setup(VerifyApplication* state) 
    {
      return true;
    }

    virtual void render_block(size_t i, size_t n) = 0;

    float benchmark(VerifyApplication* state)
    {
      double t0 = getSeconds();
      parallel_for((size_t)N/dN, [&](size_t i) {
          render_block(i*dN, dN);
      });
      double t1 = getSeconds();
      return 1E-6f * (float)(N)/float(t1-t0);
    }

    virtual void cleanup(VerifyApplication* state) 
    {
    }
  };

  struct CoherentRaysBenchmark : public ParallelIntersectBenchmark
  {
    GeometryType gtype;
    SceneFlags sflags;
    RTCBuildQuality quality;
    IntersectMode imode;
    IntersectVariant ivariant;
    size_t numPhi;
    RTCDeviceRef device;
    Ref<VerifyScene> scene;
	static const size_t tileSizeX = 32;
	static const size_t tileSizeY = 32;
	static const size_t width = 4096;
	static const size_t height = 4096;
	static const size_t numTilesX = width / tileSizeX;
	static const size_t numTilesY = height / tileSizeY;
    
    CoherentRaysBenchmark (std::string name, int isa, GeometryType gtype, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant, size_t numPhi)
      : ParallelIntersectBenchmark(name,isa,numTilesX*numTilesY,1), gtype(gtype), sflags(sflags), quality(quality), imode(imode), ivariant(ivariant), numPhi(numPhi) {}
    
    size_t setNumPrimitives(size_t N) 
    { 
      numPhi = size_t(ceilf(sqrtf(N/4.0f)));
      return 4*numPhi*numPhi;
    }

    bool setup(VerifyApplication* state) 
    {
      if (!ParallelIntersectBenchmark::setup(state))
        return false;

      std::string cfg = state->rtcore + ",start_threads=1,set_affinity=1,isa="+stringOfISA(isa);
      device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      rtcSetDeviceErrorFunction(device,errorHandler,nullptr);
      if (!supportsIntersectMode(device,imode))
        return false;

      scene = new VerifyScene(device,sflags);
      switch (gtype) {
      case TRIANGLE_MESH:    scene->addGeometry(quality,SceneGraph::createTriangleSphere(zero,one,numPhi)); break;
      case TRIANGLE_MESH_MB: scene->addGeometry(quality,SceneGraph::createTriangleSphere(zero,one,numPhi)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case QUAD_MESH:        scene->addGeometry(quality,SceneGraph::createQuadSphere(zero,one,numPhi)); break;
      case QUAD_MESH_MB:     scene->addGeometry(quality,SceneGraph::createQuadSphere(zero,one,numPhi)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case SUBDIV_MESH:      scene->addGeometry(quality,SceneGraph::createSubdivSphere(zero,one,8,float(numPhi)/8.0f)); break;
      case SUBDIV_MESH_MB:   scene->addGeometry(quality,SceneGraph::createSubdivSphere(zero,one,8,float(numPhi)/8.0f)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case SPHERE_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::SPHERE)); break;
      case SPHERE_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::SPHERE)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case DISC_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::DISC)); break;
      case DISC_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::DISC)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case ORIENTED_DISC_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::ORIENTED_DISC)); break;
      case ORIENTED_DISC_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::ORIENTED_DISC)->set_motion_vector(random_motion_vector2(0.01f))); break;
      default:               throw std::runtime_error("invalid geometry for benchmark");
      }
      rtcCommitScene (*scene);
      AssertNoError(device);
            
      return true;
    }

    void render_block(size_t i, size_t)
    {
      float rcpWidth = 1.0f/width;
      float rcpHeight = 1.0/height;
      const size_t tileY = i / numTilesX;
	  const size_t tileX = i - tileY * numTilesX;
	  const size_t x0 = tileX * tileSizeX;
	  const size_t x1 = min(x0 + tileSizeX, width);
	  const size_t y0 = tileY * tileSizeY;
	  const size_t y1 = min(y0 + tileSizeY, height);
      
      RTCIntersectContext context;
      rtcInitIntersectContext(&context);
      context.flags = ((ivariant & VARIANT_COHERENT_INCOHERENT_MASK) == VARIANT_COHERENT) ? RTC_INTERSECT_CONTEXT_FLAG_COHERENT :  RTC_INTERSECT_CONTEXT_FLAG_INCOHERENT;

      switch (imode) 
      {
      case MODE_INTERSECT1: 
      {
        for (size_t y=y0; y<y1; y++) {
          for (size_t x=x0; x<x1; x++) {
            RTCRayHit ray = fastMakeRay(zero,Vec3f(float(x)*rcpWidth,1,float(y)*rcpHeight));
            switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
            case VARIANT_INTERSECT: rtcIntersect1(*scene,&context,&ray); break;
            case VARIANT_OCCLUDED : rtcOccluded1 (*scene,&context,(RTCRay*)&ray); break;
            }
          }
        }
        break;
      }
      case MODE_INTERSECT4: 
      {
        for (size_t y=y0; y<y1; y+=2) {
          for (size_t x=x0; x<x1; x+=2) {
            RTCRayHit4 ray4; 
            for (size_t dy=0; dy<2; dy++) {
              for (size_t dx=0; dx<2; dx++) {
                setRay(ray4,2*dy+dx,fastMakeRay(zero,Vec3f(float(x+dx)*rcpWidth,1,float(y+dy)*rcpHeight)));
              }
            }
            __aligned(16) int valid4[4] = { -1,-1,-1,-1 };
            switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
            case VARIANT_INTERSECT: rtcIntersect4(valid4,*scene,&context,&ray4); break;
            case VARIANT_OCCLUDED : rtcOccluded4 (valid4,*scene,&context,(RTCRay4*)&ray4); break;
            }
          }
        }
        break;
      }
      case MODE_INTERSECT8: 
      {
        for (size_t y=y0; y<y1; y+=4) {
          for (size_t x=x0; x<x1; x+=2) {
            RTCRayHit8 ray8; 
            for (size_t dy=0; dy<4; dy++) {
              for (size_t dx=0; dx<2; dx++) {
                setRay(ray8,2*dy+dx,fastMakeRay(zero,Vec3f(float(x+dx)*rcpWidth,1,float(y+dy)*rcpHeight)));
              }
            }
            __aligned(32) int valid8[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
            switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
            case VARIANT_INTERSECT: rtcIntersect8(valid8,*scene,&context,&ray8); break;
            case VARIANT_OCCLUDED : rtcOccluded8 (valid8,*scene,&context,(RTCRay8*)&ray8); break;
            }
          }
        }
        break;
      }
      case MODE_INTERSECT16: 
      {
        for (size_t y=y0; y<y1; y+=4) {
          for (size_t x=x0; x<x1; x+=4) {
            RTCRayHit16 ray16; 
            for (size_t dy=0; dy<4; dy++) {
              for (size_t dx=0; dx<4; dx++) {
                setRay(ray16,4*dy+dx,fastMakeRay(zero,Vec3f(float(x+dx)*rcpWidth,1,float(y+dy)*rcpHeight)));
              }
            }
            __aligned(64) int valid16[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
            switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
            case VARIANT_INTERSECT: rtcIntersect16(valid16,*scene,&context,&ray16); break;
            case VARIANT_OCCLUDED : rtcOccluded16 (valid16,*scene,&context,(RTCRay16*)&ray16); break;
            }
          }
        }
        break;
      }
      case MODE_INTERSECT1M: 
      {
        for (size_t y=y0; y<y1; y+=16) {
          for (size_t x=x0; x<x1; x+=16) {
            RTCRayHit rays[256];
            for (size_t dy=0; dy<16; dy++) {
              for (size_t dx=0; dx<16; dx++) {
                rays[dy*16+dx] = fastMakeRay(zero,Vec3f(float(x+dx)*rcpWidth,1,float(y+dy)*rcpHeight));
              }
            }
            switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
            case VARIANT_INTERSECT: rtcIntersect1M(*scene,&context,rays,256,sizeof(RTCRayHit)); break;
            case VARIANT_OCCLUDED : rtcOccluded1M (*scene,&context,(RTCRay*)rays,256,sizeof(RTCRayHit)); break;
            }
          }
        }
        break;
      }
      default: break;
      }
    }

    float benchmark(VerifyApplication* state) {
      return tileSizeX*tileSizeY*ParallelIntersectBenchmark::benchmark(state);
    }

    virtual void cleanup(VerifyApplication* state) 
    {
      AssertNoError(device);
      scene = nullptr;
      device = nullptr;
      ParallelIntersectBenchmark::cleanup(state);
    }
  };

  struct IncoherentRaysBenchmark : public ParallelIntersectBenchmark
  {
    GeometryType gtype;
    SceneFlags sflags;
    RTCBuildQuality quality;
    IntersectMode imode;
    IntersectVariant ivariant;
    size_t numPhi;
    RTCDeviceRef device;
    Ref<VerifyScene> scene;
    static const size_t numRays = 16*1024*1024;
    static const size_t deltaRays = 1024;
    
    IncoherentRaysBenchmark (std::string name, int isa, GeometryType gtype, SceneFlags sflags, RTCBuildQuality quality, IntersectMode imode, IntersectVariant ivariant, size_t numPhi)
      : ParallelIntersectBenchmark(name,isa,numRays,deltaRays), gtype(gtype), sflags(sflags), quality(quality), imode(imode), ivariant(ivariant), numPhi(numPhi), device(nullptr)  {}

    size_t setNumPrimitives(size_t N) 
    { 
      numPhi = size_t(ceilf(sqrtf(N/4.0f)));
      return 4*numPhi*numPhi;
    }

    bool setup(VerifyApplication* state) 
    {
      if (!ParallelIntersectBenchmark::setup(state))
        return false;

      std::string cfg = state->rtcore + ",start_threads=1,set_affinity=1,isa="+stringOfISA(isa);
      device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      rtcSetDeviceErrorFunction(device,errorHandler,nullptr);
      if (!supportsIntersectMode(device,imode))
        return false;

      scene = new VerifyScene(device,sflags);
      switch (gtype) {
      case TRIANGLE_MESH:    scene->addGeometry(quality,SceneGraph::createTriangleSphere(zero,one,numPhi)); break;
      case TRIANGLE_MESH_MB: scene->addGeometry(quality,SceneGraph::createTriangleSphere(zero,one,numPhi)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case QUAD_MESH:        scene->addGeometry(quality,SceneGraph::createQuadSphere(zero,one,numPhi)); break;
      case QUAD_MESH_MB:     scene->addGeometry(quality,SceneGraph::createQuadSphere(zero,one,numPhi)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case SUBDIV_MESH:      scene->addGeometry(quality,SceneGraph::createSubdivSphere(zero,one,8,float(numPhi)/8.0f)); break;
      case SUBDIV_MESH_MB:   scene->addGeometry(quality,SceneGraph::createSubdivSphere(zero,one,8,float(numPhi)/8.0f)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case SPHERE_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::SPHERE)); break;
      case SPHERE_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::SPHERE)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case DISC_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::DISC)); break;
      case DISC_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::DISC)->set_motion_vector(random_motion_vector2(0.01f))); break;
      case ORIENTED_DISC_GEOMETRY:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::ORIENTED_DISC)); break;
      case ORIENTED_DISC_GEOMETRY_MB:  scene->addGeometry(quality,SceneGraph::createPointSphere(zero, one, float(one)/100.f, numPhi, SceneGraph::ORIENTED_DISC)->set_motion_vector(random_motion_vector2(0.01f))); break;
      default:               throw std::runtime_error("invalid geometry for benchmark");
      }
      rtcCommitScene (*scene);
      AssertNoError(device);      

      return true;
    }

    void render_block(size_t i, size_t dn)
    {
      RTCIntersectContext context;
      rtcInitIntersectContext(&context);
      context.flags = ((ivariant & VARIANT_COHERENT_INCOHERENT_MASK) == VARIANT_COHERENT) ? RTC_INTERSECT_CONTEXT_FLAG_COHERENT :  RTC_INTERSECT_CONTEXT_FLAG_INCOHERENT;

      RandomSampler sampler;
      RandomSampler_init(sampler, (int)i);

      switch (imode) 
      {
      case MODE_INTERSECT1: 
      {
        for (size_t j=0; j<dn; j++) {
          RTCRayHit ray; 
          fastMakeRay(ray,zero,sampler);
          switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
          case VARIANT_INTERSECT: rtcIntersect1(*scene,&context,&ray); break;
          case VARIANT_OCCLUDED : rtcOccluded1 (*scene,&context,(RTCRay*)&ray); break;
          }
        }
        break;
      }
      case MODE_INTERSECT4: 
      {
        for (size_t j=0; j<dn; j+=4) {
          RTCRayHit4 ray4;
          for (size_t k=0; k<4; k++) {
            setRay(ray4,k,fastMakeRay(zero,sampler));
          }
          __aligned(16) int valid4[4] = { -1,-1,-1,-1 };
          switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
          case VARIANT_INTERSECT: rtcIntersect4(valid4,*scene,&context,&ray4); break;
          case VARIANT_OCCLUDED : rtcOccluded4 (valid4,*scene,&context,(RTCRay4*)&ray4); break;
          }
        }
        break;
      }
      case MODE_INTERSECT8: 
      {
        for (size_t j=0; j<dn; j+=8) {
          RTCRayHit8 ray8;
          for (size_t k=0; k<8; k++) {
            setRay(ray8,k,fastMakeRay(zero,sampler));
          }
          __aligned(32) int valid8[8] = { -1,-1,-1,-1,-1,-1,-1,-1 };
          switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
          case VARIANT_INTERSECT: rtcIntersect8(valid8,*scene,&context,&ray8); break;
          case VARIANT_OCCLUDED : rtcOccluded8 (valid8,*scene,&context,(RTCRay8*)&ray8); break;
          }
        }
        break;
      }
      case MODE_INTERSECT16: 
      {
        for (size_t j=0; j<dn; j+=16) {
          RTCRayHit16 ray16;
          for (size_t k=0; k<16; k++) {
            setRay(ray16,k,fastMakeRay(zero,sampler));
          }
          __aligned(64) int valid16[16] = { -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1 };
          switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
          case VARIANT_INTERSECT: rtcIntersect16(valid16,*scene,&context,&ray16); break;
          case VARIANT_OCCLUDED : rtcOccluded16 (valid16,*scene,&context,(RTCRay16*)&ray16); break;
          }
        }
        break;
      }
      case MODE_INTERSECT1M: 
      {
        for (size_t j=0; j<dn; j+=128) {
          RTCRayHit rays[128];
          for (size_t k=0; k<128; k++) fastMakeRay(rays[k],zero,sampler);
          switch (ivariant & VARIANT_INTERSECT_OCCLUDED_MASK) {
          case VARIANT_INTERSECT: rtcIntersect1M(*scene,&context,rays,128,sizeof(RTCRayHit)); break;
          case VARIANT_OCCLUDED : rtcOccluded1M (*scene,&context,(RTCRay*)rays,128,sizeof(RTCRayHit)); break;
          }
        }
        break;
      }
      default: break;
      }
    }

    virtual void cleanup(VerifyApplication* state) 
    {
      AssertNoError(device);
      scene = nullptr;
      device = nullptr;
      ParallelIntersectBenchmark::cleanup(state);
    }
  };

  static std::atomic<ssize_t> create_geometry_bytes_used(0);

  struct CreateGeometryBenchmark : public VerifyApplication::Benchmark
  {
    GeometryType gtype;
    SceneFlags sflags;
    RTCBuildQuality quality;
    size_t numPhi;
    size_t numMeshes;
    bool update;
    bool dobenchmark; // true = measure build performance, false = measure memory consumption
    size_t numPrimitives;
    RTCDeviceRef device;
    Ref<VerifyScene> scene;
    std::vector<Ref<SceneGraph::Node>> geometries;
    
    CreateGeometryBenchmark (std::string name, int isa, GeometryType gtype, SceneFlags sflags, RTCBuildQuality quality, size_t numPhi, size_t numMeshes, bool update, bool dobenchmark)
      : VerifyApplication::Benchmark(name,isa,dobenchmark ? "Mprims/s" : "MB",dobenchmark,dobenchmark?10:1), gtype(gtype), sflags(sflags), quality(quality), 
        numPhi(numPhi), numMeshes(numMeshes), update(update), dobenchmark(dobenchmark),
        numPrimitives(0), device(nullptr), scene(nullptr) {}

    size_t setNumPrimitives(size_t N) 
    { 
      numPhi = (size_t) ceilf(sqrtf(N/4.0f));
      return 4*numPhi*numPhi;
    }

    void create_scene()
    {
      scene = new VerifyScene(device,sflags);

      numPrimitives = 0;
      for (size_t i=0; i<numMeshes; i++)
      {
        numPrimitives += geometries[i]->numPrimitives();

        switch (gtype) { 
        case TRIANGLE_MESH:    
        case QUAD_MESH:        
        case SUBDIV_MESH:      
        case BEZIER_GEOMETRY:
        case BSPLINE_GEOMETRY:
        case CATMULL_GEOMETRY:
        case LINE_GEOMETRY:
        case TRIANGLE_MESH_MB: 
        case QUAD_MESH_MB:     
        case SUBDIV_MESH_MB:   
        case BEZIER_GEOMETRY_MB:
        case BSPLINE_GEOMETRY_MB:
        case CATMULL_GEOMETRY_MB:
        case LINE_GEOMETRY_MB:
          scene->addGeometry(quality,geometries[i]); 
          break;
        default: 
          throw std::runtime_error("invalid geometry for benchmark");
        }
      }
    }

    static bool memoryMonitor(void* userPtr, const ssize_t bytes, const bool /*post*/)
    {
      create_geometry_bytes_used += bytes;
      return true;
    }

    bool setup(VerifyApplication* state) 
    {
      std::string cfg = "start_threads=1,set_affinity=1,isa="+stringOfISA(isa) + ",threads=" + std::to_string((long long)numThreads)+","+state->rtcore;
      device = rtcNewDevice(cfg.c_str());
      errorHandler(nullptr,rtcGetDeviceError(device));
      rtcSetDeviceErrorFunction(device,errorHandler,nullptr);
      if (!dobenchmark) rtcSetDeviceMemoryMonitorFunction(device,memoryMonitor,nullptr);

      for (unsigned int i=0; i<numMeshes; i++)
      {
        switch (gtype) {
        case TRIANGLE_MESH:    
        case TRIANGLE_MESH_MB: geometries.push_back(SceneGraph::createTriangleSphere(zero,float(i+1),numPhi)); break;
        case QUAD_MESH:        
        case QUAD_MESH_MB:     geometries.push_back(SceneGraph::createQuadSphere(zero,float(i+1),numPhi)); break;
        case SUBDIV_MESH:      
        case SUBDIV_MESH_MB:   geometries.push_back(SceneGraph::createSubdivSphere(zero,float(i+1),8,float(numPhi)/8.0f)); break;
        case BEZIER_GEOMETRY:    
        case BEZIER_GEOMETRY_MB:
        case BSPLINE_GEOMETRY:
        case BSPLINE_GEOMETRY_MB:
        case CATMULL_GEOMETRY:
        case CATMULL_GEOMETRY_MB: geometries.push_back(SceneGraph::createHairyPlane(i,Vec3fa(float(i)),Vec3fa(1,0,0),Vec3fa(0,1,0),0.01f,0.00001f,4*numPhi*numPhi,SceneGraph::FLAT_CURVE)); break;
        case LINE_GEOMETRY: 
        case LINE_GEOMETRY_MB: geometries.push_back(SceneGraph::createHairyPlane(i,Vec3fa(float(i)),Vec3fa(1,0,0),Vec3fa(0,1,0),0.01f,0.00001f,4*numPhi*numPhi/3,SceneGraph::FLAT_CURVE)); break;
        default:               throw std::runtime_error("invalid geometry for benchmark");
        }

        switch (gtype) {
        case LINE_GEOMETRY:    
        case LINE_GEOMETRY_MB: geometries.back() = SceneGraph::convert_bezier_to_lines(geometries.back()); break;
        default: break;
        }

        switch (gtype) {
        case TRIANGLE_MESH_MB: 
        case QUAD_MESH_MB:     
        case SUBDIV_MESH_MB:   
        case BEZIER_GEOMETRY_MB:
        case BSPLINE_GEOMETRY_MB:
        case CATMULL_GEOMETRY_MB:
        case LINE_GEOMETRY_MB: geometries.back() = geometries.back()->set_motion_vector(random_motion_vector2(0.0001f)); break;
        default: break;
        }
      }

      if (update)
        create_scene();

      return true;
    }

    float benchmark(VerifyApplication* state)
    {
      if (!update) create_scene();
      
      double t0 = getSeconds();
      
      if (update)
        for (unsigned int i=0; i<numMeshes; i++) 
          rtcCommitGeometry(rtcGetGeometry(*scene,i));
      
      create_geometry_bytes_used = 0;
      rtcCommitScene (*scene);
      AssertNoError(device);
      
      double t1 = getSeconds();

      if (dobenchmark)
        return 1E-6f*float(numPrimitives)/float(t1-t0);
      else
        return 1E-6f*create_geometry_bytes_used;
    }
      
    virtual void cleanup(VerifyApplication* state) 
    {
      scene = nullptr;
      device = nullptr;
      geometries.clear();
    }
  };

  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////
  /////////////////////////////////////////////////////////////////////////////////

  VerifyApplication::VerifyApplication ()
    : Application(Application::FEATURE_RTCORE), 
      intensity(1.0f), 
      numPassedTests(0), numFailedTests(0), numFailedAndIgnoredTests(0),
      tests(new TestGroup("",false,false)), 
      device(nullptr),
      user_specified_tests(false), flatten(true), parallel(true), cdash(false), 
      database(""), update_database(false), benchmark_tolerance(0.05f),
      usecolors(true)
  {
    rtcore = ""; // do not start threads nor set affinty for normal tests 
    device = rtcNewDevice(rtcore.c_str());

#if defined(__WIN32__)
    usecolors = false;
#endif

    GeometryType gtypes[] = { TRIANGLE_MESH, TRIANGLE_MESH_MB, GRID_MESH, GRID_MESH_MB, QUAD_MESH, QUAD_MESH_MB, SUBDIV_MESH, SUBDIV_MESH_MB };
    GeometryType gtypes_all[] = { TRIANGLE_MESH, TRIANGLE_MESH_MB, QUAD_MESH, QUAD_MESH_MB, GRID_MESH, GRID_MESH_MB, SUBDIV_MESH, SUBDIV_MESH_MB, 
                                  BEZIER_GEOMETRY, BEZIER_GEOMETRY_MB, BSPLINE_GEOMETRY, BSPLINE_GEOMETRY_MB, CATMULL_GEOMETRY, CATMULL_GEOMETRY_MB,
                                  LINE_GEOMETRY, LINE_GEOMETRY_MB };

    /* create list of all ISAs to test */
#if defined(EMBREE_TARGET_SSE2)
    if (hasISA(SSE2)) isas.push_back(SSE2);
#endif
#if defined(EMBREE_TARGET_SSE42)
    if (hasISA(SSE42)) isas.push_back(SSE42);
#endif
#if defined(EMBREE_TARGET_AVX)
    if (hasISA(AVX)) isas.push_back(AVX);
#endif
#if defined(EMBREE_TARGET_AVX2)
    if (hasISA(AVX2)) isas.push_back(AVX2);
#endif
#if defined(EMBREE_TARGET_AVX512)
    if (hasISA(AVX512)) isas.push_back(AVX512);
#endif
    
    /* create list of all intersect modes to test */
    intersectModes.push_back(MODE_INTERSECT1);
    intersectModes.push_back(MODE_INTERSECT4);
    intersectModes.push_back(MODE_INTERSECT8);
    intersectModes.push_back(MODE_INTERSECT16);
    intersectModes.push_back(MODE_INTERSECT1M);
    intersectModes.push_back(MODE_INTERSECT1Mp);
    intersectModes.push_back(MODE_INTERSECTNM1);
    intersectModes.push_back(MODE_INTERSECTNM3);
    intersectModes.push_back(MODE_INTERSECTNM4);
    intersectModes.push_back(MODE_INTERSECTNM8);
    intersectModes.push_back(MODE_INTERSECTNM16);
    intersectModes.push_back(MODE_INTERSECTNp);
    
    /* create a list of all intersect variants for each intersect mode */
    intersectVariants.push_back(VARIANT_INTERSECT_COHERENT);
    intersectVariants.push_back(VARIANT_OCCLUDED_COHERENT);
    intersectVariants.push_back(VARIANT_INTERSECT_INCOHERENT);
    intersectVariants.push_back(VARIANT_OCCLUDED_INCOHERENT);
    intersectVariants.push_back(VARIANT_INTERSECT_OCCLUDED_COHERENT);

    /* create list of all scene flags to test */
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_NONE,       RTC_BUILD_QUALITY_MEDIUM));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_MEDIUM));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_COMPACT,       RTC_BUILD_QUALITY_MEDIUM));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_MEDIUM));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_NONE,       RTC_BUILD_QUALITY_HIGH));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,       RTC_BUILD_QUALITY_LOW));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,       RTC_BUILD_QUALITY_MEDIUM));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_LOW));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_COMPACT,       RTC_BUILD_QUALITY_LOW));
    sceneFlags.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_LOW));

    sceneFlagsRobust.push_back(SceneFlags(RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_MEDIUM));
    sceneFlagsRobust.push_back(SceneFlags(RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_MEDIUM));
    sceneFlagsRobust.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_LOW));
    sceneFlagsRobust.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_LOW));

    sceneFlagsDynamic.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,       RTC_BUILD_QUALITY_LOW));
    sceneFlagsDynamic.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,       RTC_BUILD_QUALITY_MEDIUM));
    sceneFlagsDynamic.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_LOW));
    sceneFlagsDynamic.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_COMPACT,       RTC_BUILD_QUALITY_LOW));
    sceneFlagsDynamic.push_back(SceneFlags(RTC_SCENE_FLAG_DYNAMIC | RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_LOW));

    /**************************************************************************/
    /*                      Smaller API Tests                                 */
    /**************************************************************************/

    std::stack<Ref<TestGroup>> groups;
    groups.push(tests.dynamicCast<TestGroup>());
    auto push = [&] (Ref<TestGroup> group) {
      groups.top()->add(group.dynamicCast<Test>());
      groups.push(group);
    };

    groups.top()->add(new DeviceCreationTest("create_device"));
    
    /* add Embree internal tests */
    for (size_t i=2000000; i<3000000; i++) {
      const char* testName = (const char*) rtcGetDeviceProperty(device,(RTCDeviceProperty)i);
      if (testName == nullptr) break;
      groups.top()->add(new EmbreeInternalTest(testName,i-2000000));
    }
    groups.top()->add(new os_shrink_test());

    for (auto isa : isas)
    {
      push(new TestGroup(stringOfISA(isa),false,false));
      
      groups.top()->add(new MultipleDevicesTest("multiple_devices",isa));
      groups.top()->add(new TypesTest("types_test",isa));

      push(new TestGroup("get_bounds",true,true));
      for (auto gtype : gtypes_all)
        groups.top()->add(new GetBoundsTest(to_string(gtype),isa,gtype));
      groups.pop();

      push(new TestGroup("get_linear_bounds",true,true));
      for (auto gtype : gtypes_all)
        groups.top()->add(new GetLinearBoundsTest(to_string(gtype),isa,gtype));
      groups.pop();
      
      groups.top()->add(new GetUserDataTest("get_user_data",isa));

      push(new TestGroup("buffer_stride",true,true));
      for (auto gtype : gtypes)
        groups.top()->add(new BufferStrideTest(to_string(gtype),isa,gtype));
      groups.pop();
      
      /**************************************************************************/
      /*                        Builder Tests                                   */
      /**************************************************************************/
      
      push(new TestGroup("empty_scene",true,true));
      for (auto sflags : sceneFlags) 
        groups.top()->add(new EmptySceneTest(to_string(sflags),isa,sflags));
      groups.pop();
      
      push(new TestGroup("empty_geometry",true,true));
      for (auto sflags : sceneFlags) 
        groups.top()->add(new EmptyGeometryTest(to_string(sflags),isa,sflags,RTC_BUILD_QUALITY_MEDIUM));
      groups.pop();
      
      push(new TestGroup("many_build",false,false,false));
      for (auto sflags : sceneFlags) 
        groups.top()->add(new ManyBuildTest(to_string(sflags),isa,sflags,RTC_BUILD_QUALITY_MEDIUM));
      groups.pop();

      push(new TestGroup("build",true,true));
      for (auto sflags : sceneFlags) 
        groups.top()->add(new BuildTest(to_string(sflags),isa,sflags,RTC_BUILD_QUALITY_MEDIUM));
      groups.pop();
      
      push(new TestGroup("overlapping_primitives",true,false));
      for (auto sflags : sceneFlags)
        groups.top()->add(new OverlappingGeometryTest(to_string(sflags),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,clamp(int(intensity*10000),1000,100000)));
      groups.pop();

      push(new TestGroup("new_delete_geometry",true,true));
      for (auto sflags : sceneFlagsDynamic) 
        groups.top()->add(new NewDeleteGeometryTest(to_string(sflags),isa,sflags));
      groups.pop();

      push(new TestGroup("user_geometry_id",true,true));
      for (auto sflags : sceneFlagsDynamic) 
        groups.top()->add(new UserGeometryIDTest(to_string(sflags),isa,sflags));
      groups.pop();
      
      push(new TestGroup("enable_disable_geometry",true,true));
      for (auto sflags : sceneFlagsDynamic) 
        groups.top()->add(new EnableDisableGeometryTest(to_string(sflags),isa,sflags));
      groups.pop();
      
      push(new TestGroup("update",true,true));
      for (auto sflags : sceneFlagsDynamic) {
        for (auto imode : intersectModes) {
          for (auto ivariant : intersectVariants) {
            if (has_variant(imode,ivariant)) {
              groups.top()->add(new UpdateTest("deformable."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_REFIT,imode,ivariant));
              groups.top()->add(new UpdateTest("dynamic."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_LOW,imode,ivariant));
            }
          }
        }
      }
      groups.pop();

#if !defined(TASKING_PPL) // FIXME: PPL has some issues here!
      groups.top()->add(new GarbageGeometryTest("build_garbage_geom",isa));
#endif

      GeometryType gtypes_memory[] = { TRIANGLE_MESH, TRIANGLE_MESH_MB, QUAD_MESH, QUAD_MESH_MB, BEZIER_GEOMETRY, BEZIER_GEOMETRY_MB, LINE_GEOMETRY, LINE_GEOMETRY_MB };
      std::vector<std::pair<SceneFlags,RTCBuildQuality>> sflags_quality_memory;
      sflags_quality_memory.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_NONE   ,RTC_BUILD_QUALITY_MEDIUM), RTC_BUILD_QUALITY_MEDIUM));
      sflags_quality_memory.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_MEDIUM), RTC_BUILD_QUALITY_MEDIUM));
      sflags_quality_memory.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_ROBUST ,RTC_BUILD_QUALITY_MEDIUM), RTC_BUILD_QUALITY_MEDIUM));
      sflags_quality_memory.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,RTC_BUILD_QUALITY_LOW)   , RTC_BUILD_QUALITY_LOW));

      if (intensity > 1.0f)
      {
        push(new TestGroup("memory_consumption",false,false));
        
        for (auto gtype : gtypes_memory)
          for (auto sflags : sflags_quality_memory)
            groups.top()->add(new MemoryConsumptionTest(to_string(gtype)+"."+to_string(sflags.first,sflags.second),isa,gtype,sflags.first,sflags.second));
        
        groups.pop();
      }

      /**************************************************************************/
      /*                     Interpolation Tests                                */
      /**************************************************************************/
      
      push(new TestGroup("interpolate",false,false));
      int interpolateTests[] = { 4,5,8,11,12,15 };

      push(new TestGroup("triangles",true,true));
      for (auto s : interpolateTests) 
        groups.top()->add(new InterpolateTrianglesTest(std::to_string((long long)(s)),isa,s));
      groups.pop();

      push(new TestGroup("grid",true,true));
      for (auto s : interpolateTests) 
        groups.top()->add(new InterpolateGridTest(std::to_string((long long)(s)),isa,s));
      groups.pop();

      push(new TestGroup("subdiv",true,true));
      for (auto s : interpolateTests)
        groups.top()->add(new InterpolateSubdivTest(std::to_string((long long)(s)),isa,s));
      groups.pop();
        
      push(new TestGroup("hair",true,true));
      for (auto s : interpolateTests) 
        groups.top()->add(new InterpolateHairTest(std::to_string((long long)(s)),isa,s));
      groups.pop();

      groups.pop();
      
      /**************************************************************************/
      /*                      Intersection Tests                                */
      /**************************************************************************/

      push(new TestGroup("triangle_hit",true,true));
      for (auto sflags : sceneFlags) 
        for (auto imode : intersectModes) 
          for (auto ivariant : intersectVariants)
            if (has_variant(imode,ivariant))
                groups.top()->add(new TriangleHitTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
      groups.pop();
      
      push(new TestGroup("quad_hit",true,true));
      for (auto sflags : sceneFlags) 
        for (auto imode : intersectModes) 
          for (auto ivariant : intersectVariants)
            if (has_variant(imode,ivariant))
                groups.top()->add(new QuadHitTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
      groups.pop();

      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED)) 
      {
        push(new TestGroup("ray_masks",true,true));
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant))
                  groups.top()->add(new RayMasksTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
        groups.pop();
      }
      
      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_BACKFACE_CULLING_ENABLED)) 
      {
        push(new TestGroup("backface_culling",true,true));
        for (auto gtype : gtypes)
          for (auto sflags : sceneFlags) 
            for (auto imode : intersectModes) 
              for (auto ivariant : intersectVariants)
                if (has_variant(imode,ivariant))
                    groups.top()->add(new BackfaceCullingTest(to_string(gtype,sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,gtype,imode,ivariant));
        groups.pop();
      }
      
      push(new TestGroup("intersection_filter",true,true));
      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_FILTER_FUNCTION_SUPPORTED)) 
      {
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant)) {
                groups.top()->add(new IntersectionFilterTest("triangles."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,false,imode,ivariant));
              }
        
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant))
                  groups.top()->add(new IntersectionFilterTest("subdiv."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,true,imode,ivariant));
      }
      groups.pop();

      push(new TestGroup("instancing",true,true));
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant)) 
                groups.top()->add(new InstancingTest("instancing."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,false,imode,ivariant));
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant)) 
                groups.top()->add(new InstancingTest("instancing."+to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,true,imode,ivariant));
      groups.pop();
      
      push(new TestGroup("inactive_rays",true,true));
      for (auto sflags : sceneFlags) 
        for (auto imode : intersectModes) 
          for (auto ivariant : intersectVariants)
            if (has_variant(imode,ivariant))
                if (imode != MODE_INTERSECT1) // INTERSECT1 does not support disabled rays
                  groups.top()->add(new InactiveRaysTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
      groups.pop();
      
      push(new TestGroup("watertight_triangles",true,true)); {
        std::string watertightModels [] = {"sphere.triangles", "plane.triangles"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }

      push(new TestGroup("watertight_triangles_mb",true,true)); {
        std::string watertightModels [] = {"sphere.triangles_mb", "plane.triangles_mb"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }

      push(new TestGroup("watertight_quads",true,true)); {
        std::string watertightModels [] = {"sphere.quads", "plane.quads"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }

      push(new TestGroup("watertight_quads_mb",true,true)); {
        std::string watertightModels [] = {"sphere.quads_mb", "plane.quads_mb"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }

       push(new TestGroup("watertight_grids",true,true)); {
        std::string watertightModels [] = {"sphere.grids", "plane.grids"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }

      push(new TestGroup("watertight_grids_mb",true,true)); {
        std::string watertightModels [] = {"sphere.grids_mb", "plane.grids_mb"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }
      
      push(new TestGroup("watertight_subdiv",true,true)); {
        std::string watertightModels [] = { "sphere.subdiv", "plane.subdiv"};
        const Vec3fa watertight_pos = Vec3fa(148376.0f,1234.0f,-223423.0f);
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new WatertightTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model,watertight_pos));
        groups.pop();
      }
      
      /*push(new TestGroup("small_triangle_hit_test",true,true)); {
        const Vec3fa pos = Vec3fa(0.0f,0.0f,0.0f);
        const float radius = 1000000.0f;
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            groups.top()->add(new SmallTriangleHitTest(to_string(sflags,imode),isa,sflags,imode,pos,radius));
        groups.pop();
        }*/
      
      push(new TestGroup("ray_alignment_test",true,true)); {
        std::string watertightModels [] = {"sphere.triangles", "sphere.quads", "sphere.grids", "sphere.subdiv" };
        for (auto sflags : sceneFlagsRobust) 
          for (auto imode : intersectModes) 
            for (std::string model : watertightModels) 
              groups.top()->add(new RayAlignmentTest(to_string(sflags,imode)+"."+model,isa,sflags,imode,model));
        groups.pop();
      }

      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_IGNORE_INVALID_RAYS_ENABLED))
      {
        push(new TestGroup("nan_test",true,false));
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant))
                  groups.top()->add(new NaNTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
        groups.pop();
        
        push(new TestGroup("inf_test",true,false));
        for (auto sflags : sceneFlags) 
          for (auto imode : intersectModes) 
            for (auto ivariant : intersectVariants)
              if (has_variant(imode,ivariant))
                  groups.top()->add(new InfTest(to_string(sflags,imode,ivariant),isa,sflags,RTC_BUILD_QUALITY_MEDIUM,imode,ivariant));
        groups.pop();
      }
      
      /**************************************************************************/
      /*                      Point Query Tests                                */
      /**************************************************************************/

      push(new TestGroup("point_query",true,true));
      for (auto sflags : sceneFlags) {
        groups.top()->add(new PointQueryAPICallsTest("point_query_api_calls",isa,sflags));
        if (stringOfISA(isa) == "SSE4.1" || stringOfISA(isa) == "SSE4.2") {
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"bvh4.triangle4v"));
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"bvh4.triangle4i"));
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"qbvh4.triangle4i"));
        }
        if (stringOfISA(isa) == "AVX" || stringOfISA(isa) == "AVX2") {
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"bvh8.triangle4v"));
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"bvh8.triangle4i"));
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"qbvh8.triangle4"));
          groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags,"qbvh8.triangle4i"));
        }
        groups.top()->add(new PointQueryTest(to_string(sflags),isa,sflags));
      }

      groups.top()->add(new PointQueryMotionBlurTest("point_query_motion_blur_aligned_node",isa,SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM),"bvh4.triangle4i"));
      groups.top()->add(new PointQueryMotionBlurTest("point_query_motion_blur_quantized_node",isa,SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM),"qbvh4.triangle4i"));
      groups.top()->add(new PointQueryMotionBlurTest("point_query_motion_blur_quantized_node",isa,SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM)));
      groups.pop();
    
      /**************************************************************************/
      /*                  Randomized Stress Testing                             */
      /**************************************************************************/
      
      groups.top()->add(new IntensiveRegressionTest("regression_static",isa,rtcore_regression_static_thread,0,30));
      groups.top()->add(new IntensiveRegressionTest("regression_dynamic",isa,rtcore_regression_dynamic_thread,0,30));

      if (rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_JOIN_COMMIT_SUPPORTED)) {
	groups.top()->add(new IntensiveRegressionTest("regression_static_build_join", isa,rtcore_regression_static_thread,2,30));
	groups.top()->add(new IntensiveRegressionTest("regression_dynamic_build_join",isa,rtcore_regression_dynamic_thread,2,30));
      }

      groups.top()->add(new MemoryMonitorTest("regression_static_memory_monitor", isa,rtcore_regression_static_thread,30));
      groups.top()->add(new MemoryMonitorTest("regression_dynamic_memory_monitor",isa,rtcore_regression_dynamic_thread,30));

      /**************************************************************************/
      /*                  Function Level Testing                                */
      /**************************************************************************/

      groups.top()->add(new GeometryStateTest("geometry_state_tests", isa));
      groups.top()->add(new SceneCheckModifiedGeometryTest("scene_modified_geometry_tests", isa));
      groups.top()->add(new SphereFilterMultiHitTest("sphere_filter_multi_hit_tests", isa));

      
      /**************************************************************************/
      /*                           Benchmarks                                   */
      /**************************************************************************/
      
      push(new TestGroup("benchmarks",false,false));

      std::vector<std::pair<SceneFlags,RTCBuildQuality>> benchmark_sflags_quality;
      benchmark_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_NONE,       RTC_BUILD_QUALITY_MEDIUM),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_ROBUST,        RTC_BUILD_QUALITY_MEDIUM),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_COMPACT,       RTC_BUILD_QUALITY_MEDIUM),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_ROBUST | RTC_SCENE_FLAG_COMPACT,RTC_BUILD_QUALITY_MEDIUM),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,       RTC_BUILD_QUALITY_LOW),RTC_BUILD_QUALITY_LOW));

      std::vector<std::pair<IntersectMode,IntersectVariant>> benchmark_imodes_ivariants;
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT1,VARIANT_INTERSECT));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT1,VARIANT_OCCLUDED));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT4,VARIANT_INTERSECT));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT4,VARIANT_OCCLUDED));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT8,VARIANT_INTERSECT));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT8,VARIANT_OCCLUDED));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT16,VARIANT_INTERSECT));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT16,VARIANT_OCCLUDED));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT1M,VARIANT_INTERSECT_INCOHERENT));
      benchmark_imodes_ivariants.push_back(std::make_pair(MODE_INTERSECT1M,VARIANT_OCCLUDED_INCOHERENT));

      GeometryType benchmark_gtypes[] = { 
        TRIANGLE_MESH, 
        TRIANGLE_MESH_MB, 
        QUAD_MESH, 
        QUAD_MESH_MB, 
        SUBDIV_MESH, 
        //SUBDIV_MESH_MB  // FIXME: not supported yet
        SPHERE_GEOMETRY,
        SPHERE_GEOMETRY_MB,
        DISC_GEOMETRY,
        DISC_GEOMETRY_MB,
        ORIENTED_DISC_GEOMETRY,
        ORIENTED_DISC_GEOMETRY_MB
        // FIXME: use more geometry types
      };

      groups.top()->add(new SimpleBenchmark("simple",isa));
      
      for (auto gtype : benchmark_gtypes)
        for (auto sflags : benchmark_sflags_quality) 
          for (auto imode : benchmark_imodes_ivariants)
            groups.top()->add(new CoherentRaysBenchmark("coherent."+to_string(gtype)+"_1000k."+to_string(sflags.first,imode.first,imode.second),
                                                        isa,gtype,sflags.first,sflags.second,imode.first,imode.second,501));

      for (auto gtype : benchmark_gtypes)
        for (auto sflags : benchmark_sflags_quality) 
          for (auto imode : benchmark_imodes_ivariants)
            groups.top()->add(new IncoherentRaysBenchmark("incoherent."+to_string(gtype)+"_1000k."+to_string(sflags.first,imode.first,imode.second),
                                                          isa,gtype,sflags.first,sflags.second,imode.first,imode.second,501));

      std::vector<std::pair<SceneFlags,RTCBuildQuality>> benchmark_create_sflags_quality;
      benchmark_create_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_NONE,RTC_BUILD_QUALITY_MEDIUM),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_create_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,RTC_BUILD_QUALITY_LOW),RTC_BUILD_QUALITY_LOW));

      GeometryType benchmark_create_gtypes[] = { 
        TRIANGLE_MESH, 
        TRIANGLE_MESH_MB, 
        QUAD_MESH, 
        QUAD_MESH_MB, 
        BEZIER_GEOMETRY,
        BEZIER_GEOMETRY_MB,
        BSPLINE_GEOMETRY,
        BSPLINE_GEOMETRY_MB,
        CATMULL_GEOMETRY,
        CATMULL_GEOMETRY_MB,
        LINE_GEOMETRY,
        LINE_GEOMETRY_MB
      };

      std::vector<std::tuple<const char*,int,int>> num_primitives;
      num_primitives.push_back(std::make_tuple("120",6,1));
      num_primitives.push_back(std::make_tuple("1k" ,17,1));
      num_primitives.push_back(std::make_tuple("10k",51,1));
      num_primitives.push_back(std::make_tuple("100k",159,1));
      num_primitives.push_back(std::make_tuple("10000k_1",801,1));
      num_primitives.push_back(std::make_tuple("1000k_1",501,1));
      num_primitives.push_back(std::make_tuple("100k_10",159,10));
      num_primitives.push_back(std::make_tuple("10k_100",51,100));
      num_primitives.push_back(std::make_tuple("1k_1000",17,1000));

      for (auto gtype : benchmark_create_gtypes)
        for (auto sflags : benchmark_create_sflags_quality)
          for (auto num_prims : num_primitives)
            groups.top()->add(new CreateGeometryBenchmark("create."+to_string(gtype)+"_"+std::get<0>(num_prims)+"."+to_string(sflags.first,sflags.second),
                                                          isa,gtype,sflags.first,sflags.second,std::get<1>(num_prims),std::get<2>(num_prims),false,true));

      std::vector<std::pair<SceneFlags,RTCBuildQuality>> benchmark_update_sflags_quality;
      benchmark_update_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,RTC_BUILD_QUALITY_LOW),RTC_BUILD_QUALITY_MEDIUM));
      benchmark_update_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,RTC_BUILD_QUALITY_LOW),RTC_BUILD_QUALITY_REFIT));
      benchmark_update_sflags_quality.push_back(std::make_pair(SceneFlags(RTC_SCENE_FLAG_DYNAMIC,RTC_BUILD_QUALITY_LOW),RTC_BUILD_QUALITY_LOW));

      for (auto gtype : benchmark_create_gtypes)
        for (auto sflags : benchmark_update_sflags_quality)
          for (auto num_prims : num_primitives)
            groups.top()->add(new CreateGeometryBenchmark("update."+to_string(gtype)+"_"+std::get<0>(num_prims)+"."+to_string(sflags.first,sflags.second),
                                                          isa,gtype,sflags.first,sflags.second,std::get<1>(num_prims),std::get<2>(num_prims),true,true));

      groups.pop(); // benchmarks

      /**************************************************************************/
      /*                        Memory Consumption                              */
      /**************************************************************************/
   
      push(new TestGroup("embree_reported_memory",false,false));

      for (auto gtype : benchmark_create_gtypes)
        for (auto sflags : benchmark_create_sflags_quality)
          for (auto num_prims : num_primitives)
            groups.top()->add(new CreateGeometryBenchmark(to_string(gtype)+"_"+std::get<0>(num_prims)+"."+to_string(sflags.first,sflags.second),
                                                          isa,gtype,sflags.first,sflags.second,std::get<1>(num_prims),std::get<2>(num_prims),false,false));

      groups.pop(); // embree_reported_memory

      groups.pop(); // isa
    }

    prefix_test_names(tests);

    /* ignore failure of some tests that are known to fail */
    map_tests(tests, [&] (Ref<Test> test) { 
        if (test->name.find("watertight_subdiv") != std::string::npos) test->ignoreFailure = true;
      });
    
    /**************************************************************************/
    /*                     Command Line Parsing                               */
    /**************************************************************************/
  
    registerOption("run", [this] (Ref<ParseStream> cin, const FileName& path) {
        if (!user_specified_tests) enable_disable_all_tests(tests,false);
        user_specified_tests = true;
        std::string regex = cin->getString();
        if (!enable_disable_some_tests(tests,regex,true)) {
          std::cout << "no tests matched regular expression " << regex << std::endl;
          exit(1);
        }
      }, "--run <regexpr>: Runs all tests whose name match the regular expression. If no test matches the application fails.");

    registerOption("enable", [this] (Ref<ParseStream> cin, const FileName& path) {
        if (!user_specified_tests) enable_disable_all_tests(tests,false);
        user_specified_tests = true;
        std::string regex = cin->getString();
        enable_disable_some_tests(tests,regex,true);
      }, "--enable <regexpr>: Enables all tests whose name matches the regular expression.");

    registerOption("skip", [this] (Ref<ParseStream> cin, const FileName& path) {
        user_specified_tests = true;
        std::string regex = cin->getString();
        enable_disable_some_tests(tests,regex,false);
      }, "--skip <regexpr>: Skips all tests whose name matches the regular expression.");
    registerOptionAlias("skip","disable");

    registerOption("skip-before", [this] (Ref<ParseStream> cin, const FileName& path) {
        user_specified_tests = true;
        bool found = false;
        std::string regex = cin->getString();
        map_tests(tests, [&] (Ref<Test> test) {
            if (regex_match(test->name,regex)) found = true;
            test->enabled &= found;
          });
      }, "--skip-before <regexpr>: Skips all tests before the first test matching the regular expression.");
    registerOptionAlias("skip-before","disable-before");

    registerOption("flatten", [this] (Ref<ParseStream> cin, const FileName& path) {
        flatten = false;
      }, "--flatten: shows all leaf test names when executing tests");
    
    registerOption("sequential", [this] (Ref<ParseStream> cin, const FileName& path) {
        parallel = false;
      }, "--sequential: execute all tests sequentially");

    registerOption("parallel", [this] (Ref<ParseStream> cin, const FileName& path) {
        parallel = true;
      }, "--parallel: parallelized test execution (default)");

    registerOption("colors", [this] (Ref<ParseStream> cin, const FileName& path) {
        usecolors = true;
      }, "--colors: do use shell colors");

    registerOption("no-colors", [this] (Ref<ParseStream> cin, const FileName& path) {
        usecolors = false;
      }, "--no-colors: do not use shell colors");

    registerOption("cdash", [this] (Ref<ParseStream> cin, const FileName& path) {
        cdash = true;
      }, "--cdash: prints cdash measurements");

    registerOption("database", [this] (Ref<ParseStream> cin, const FileName& path) {
        database = cin->getString();
        update_database = true;
      }, "--database <folder>: location to folder containing the measurement database");

    registerOption("compare", [this] (Ref<ParseStream> cin, const FileName& path) {
        database = cin->getString();
        update_database = false;
      }, "--compare <folder>: compares with database, but does not update database");

    registerOption("benchmark-tolerance", [this] (Ref<ParseStream> cin, const FileName& path) {
        benchmark_tolerance = cin->getFloat();
      }, "--benchmark-tolerance: maximum relative slowdown to let a test pass");
    registerOptionAlias("benchmark-tolerance","tolerance");

    registerOption("print-tests", [this] (Ref<ParseStream> cin, const FileName& path) {
        print_tests(tests,0);
        exit(1);
      }, "--print-tests: prints all enabled test names");
    registerOptionAlias("print-tests","print-names");

    registerOption("print-ctests", [this] (Ref<ParseStream> cin, const FileName& path) {
        print_ctests(tests,0);
        exit(1);
      }, "--print-ctests: prints all test in a form to add to CMakeLists.txt");
    
    registerOption("intensity", [this] (Ref<ParseStream> cin, const FileName& path) {
        intensity = cin->getFloat();
      }, "--intensity <float>: intensity of testing to perform");

    registerOption("plot-over-primitives", [this] (Ref<ParseStream> cin, const FileName& path) {
        std::vector<Ref<Benchmark>> benchmarks;
        FileName outFileName = parse_benchmark_list(cin,benchmarks);
        plot(benchmarks,outFileName,"#primitives",1000,1100000,1.2f,0,[&] (Ref<Benchmark> benchmark, size_t& N) {
            N = benchmark->setNumPrimitives(N);
            benchmark->setup(this);
            Statistics stat = benchmark->benchmark_loop(this);
            benchmark->cleanup(this);
            return stat;
          });
        exit(1);
      }, "--plot-over-primitives <benchmark0> <benchmark1> ... <outfile>: Plots performance over number of primitives to outfile for the specified benchmarks.");

    registerOption("plot-over-threads", [this] (Ref<ParseStream> cin, const FileName& path) {
        std::vector<Ref<Benchmark>> benchmarks;
        FileName outFileName = parse_benchmark_list(cin,benchmarks);
        plot(benchmarks,outFileName,"#threads",2,getNumberOfLogicalThreads(),1.0f,2,[&] (Ref<Benchmark> benchmark, size_t N) {
            benchmark->setNumThreads(N);
            benchmark->setup(this);
            Statistics stat = benchmark->benchmark_loop(this);
            benchmark->cleanup(this);
            return stat;
          });
        exit(1);
      }, "--plot-over-primitives <benchmark0> <benchmark1> ... <outfile>: Plots performance over number of primitives to outfile for the specified benchmarks.");

    /* the internal tasking system need the device to be present to allow parallel test execution */
#if !defined(TASKING_INTERNAL)
    device = nullptr;
#endif
  }

  void VerifyApplication::prefix_test_names(Ref<Test> test, std::string prefix)
  {
    if (Ref<TestGroup> group = test.dynamicCast<TestGroup>()) 
      for (auto& t : group->tests) 
        prefix_test_names(t,group->extend_prefix(prefix));

    test->name = prefix + test->name;
    name2test[test->name] = test;
  }

  FileName VerifyApplication::parse_benchmark_list(Ref<ParseStream> cin, std::vector<Ref<Benchmark>>& benchmarks)
  {
    std::vector<std::string> regexpr;
    while (cin->peek() != "" && cin->peek()[0] != '-') 
      regexpr.push_back(cin->getString());
    
    if (regexpr.size() == 0) throw std::runtime_error("no output file specified");
    FileName outFileName = regexpr.back();
    regexpr.pop_back();
    
    for (size_t i=0; i<regexpr.size(); i++) 
    {
      map_tests(tests,[&] (Ref<Test> test) {
          if (!regex_match(test->name,regexpr[i])) return;
          Ref<Benchmark> benchmark = test.dynamicCast<Benchmark>();
          if (!benchmark) return;
          benchmarks.push_back(benchmark);
        });
    }
    return outFileName;
  }

  bool VerifyApplication::update_tests(Ref<Test> test)
  {
    if (Ref<TestGroup> group = test.dynamicCast<TestGroup>()) 
    {
      bool any_enabled = false;
      for (auto t : group->tests) any_enabled |= update_tests(t);
      return test->enabled = any_enabled;
    } 
    else 
      return test->enabled;
  }

  void VerifyApplication::print_tests(Ref<Test> test, size_t depth)
  {
    if (!test->isEnabled()) return;

    if (Ref<TestGroup> group = test.dynamicCast<TestGroup>()) 
    {
      std::cout << std::string(2*depth,' ') << group->name << (group->name != "" ? " " : "") << "{" << std::endl;
      for (auto& t : group->tests) print_tests(t,depth+1);
      std::cout << std::string(2*depth,' ') << "}" << std::endl;
    } else {
      std::cout << std::string(2*depth,' ') << test->name << std::endl;
    }
  }

  void VerifyApplication::print_ctests(Ref<Test> test, size_t depth)
  {
    if (!test->isEnabled()) return;

    if (Ref<TestGroup> group = test.dynamicCast<TestGroup>()) 
    {
      for (auto& t : group->tests) print_ctests(t,depth+1);
    } else {
      std::cout << "ADD_TEST(NAME " << test->name << " COMMAND verify --no-colors --cdash-measurements --run " << test->name << std::endl;
    }
  }

  template<typename Function> 
  void VerifyApplication::map_tests(Ref<Test> test, const Function& f)
  {
    if (Ref<TestGroup> group = test.dynamicCast<TestGroup>()) {
      for (auto& t : group->tests) map_tests(t,f);
    } else {
      f(test);
    }
  }

  void VerifyApplication::enable_disable_all_tests(Ref<Test> test, bool enabled)
  {
    map_tests(test, [&] (Ref<Test> test) { test->enabled = enabled; });
    update_tests(test);
  }

  size_t VerifyApplication::enable_disable_some_tests(Ref<Test> test, std::string regex, bool enabled)
  {
    size_t N = 0;
    map_tests(test, [&] (Ref<Test> test) { 
        if (regex_match(test->name,regex)) {
          test->enabled = enabled;
          N++;
        }
      });
   update_tests(test);
   return N;
  }

  template<typename Closure>
  void VerifyApplication::plot(std::vector<Ref<Benchmark>> benchmarks, const FileName outFileName, std::string xlabel, size_t startN, size_t endN, float f, size_t dn, const Closure& test)
  {
    std::fstream plot;
    plot.open(outFileName, std::fstream::out | std::fstream::trunc);
    plot << "set key inside right top vertical Right noreverse enhanced autotitles box linetype -1 linewidth 1.000" << std::endl;
    plot << "set samples 50, 50" << std::endl;
    plot << "set title \"" << outFileName.name() << "\"" << std::endl; 
    plot << "set xlabel \"" + xlabel + "\"" << std::endl;
    if (f != 1.0f) plot << "set logscale x" << std::endl;
    if (benchmarks.size())
      plot << "set ylabel \"" << benchmarks[0]->unit << "\"" << std::endl;
    plot << "set yrange [0:]" << std::endl;

    plot << "plot \\" << std::endl;
    for (size_t i=0; i<benchmarks.size(); i++) {
      plot << "\"" << outFileName.name() << "." << benchmarks[i]->name << ".txt\" using 1:2 title \"" << benchmarks[i]->name << "\" with lines";
      if (i != benchmarks.size()-1) plot << ",\\";
      plot << std::endl;
    }
    plot << std::endl;
    plot.close();
    
    for (auto benchmark : benchmarks) 
    {
      std::fstream data;
      data.open(outFileName.name()+"."+benchmark->name+".txt", std::fstream::out | std::fstream::trunc);
      std::cout << benchmark->name << std::endl;
      for (size_t i=startN; i<=endN; i=size_t(i*f)+dn) 
      {
        size_t N = i;
        Statistics stat = test(benchmark,N);
        data << " " << N << " " << stat.getAvg() << std::endl;
        std::cout<< " " << N << " " << stat.getAvg() << std::endl;
      }
      data.close();
    }
  }

  int VerifyApplication::main(int argc, char** argv) try
  {
    /* for best performance set FTZ and DAZ flags in MXCSR control and status register */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    /* parse command line options */
    parseCommandLine(argc,argv);

    /* run all enabled tests */
    tests->execute(this,false);

    /* print result */
    std::cout << std::endl;
    std::cout << std::setw(TEXT_ALIGN) << "Tests passed" << ": " << numPassedTests << std::endl; 
    std::cout << std::setw(TEXT_ALIGN) << "Tests failed" << ": " << numFailedTests << std::endl; 
    std::cout << std::setw(TEXT_ALIGN) << "Tests failed and ignored" << ": " << numFailedAndIgnoredTests << std::endl; 
    std::cout << std::endl;

    return (int)numFailedTests;
  }
  catch (const std::exception& e) {
    std::cout << "Error: " << e.what() << std::endl;
    return 1;
  }
  catch (...) {
    std::cout << "Error: unknown exception caught." << std::endl;
    return 1;
  }
}

int main(int argc, char** argv)
{
  embree::VerifyApplication app;
  int code = app.main(argc,argv);

  /* wait for user input under Windows when opened in separate window */
  embree::waitForKeyPressedUnderWindows();

  return code;
}

