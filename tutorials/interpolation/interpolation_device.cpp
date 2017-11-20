// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "../common/tutorial/tutorial_device.h"
#include "../common/tutorial/optics.h"

namespace embree {

//#define FORCE_FIXED_EDGE_TESSELLATION
#define FIXED_EDGE_TESSELLATION_VALUE 16

#define MAX_EDGE_LEVEL 64.0f
#define MIN_EDGE_LEVEL  4.0f
#define LEVEL_FACTOR  128.0f


/* scene data */
RTCDevice g_device = nullptr;
RTCScene g_scene = nullptr;
Vec3fa* vertex_colors = nullptr;
unsigned int triCubeID, quadCubeID;

#define NUM_VERTICES 8

__aligned(16) float cube_vertices[8][4] =
{
  { -1.0f, -1.0f, -1.0f, 0.0f },
  {  1.0f, -1.0f, -1.0f, 0.0f },
  {  1.0f, -1.0f,  1.0f, 0.0f },
  { -1.0f, -1.0f,  1.0f, 0.0f },
  { -1.0f,  1.0f, -1.0f, 0.0f },
  {  1.0f,  1.0f, -1.0f, 0.0f },
  {  1.0f,  1.0f,  1.0f, 0.0f },
  { -1.0f,  1.0f,  1.0f, 0.0f }
};

__aligned(16) float cube_vertex_colors[8][4] =
{
  {  0.0f,  0.0f,  0.0f, 0.0f },
  {  1.0f,  0.0f,  0.0f, 0.0f },
  {  1.0f,  0.0f,  1.0f, 0.0f },
  {  0.0f,  0.0f,  1.0f, 0.0f },
  {  0.0f,  1.0f,  0.0f, 0.0f },
  {  1.0f,  1.0f,  0.0f, 0.0f },
  {  1.0f,  1.0f,  1.0f, 0.0f },
  {  0.0f,  1.0f,  1.0f, 0.0f }
};

__aligned(16) float cube_vertex_crease_weights[8] = {
  inf, inf,inf, inf, inf, inf, inf, inf
};

__aligned(16) unsigned int cube_vertex_crease_indices[8] = {
  0,1,2,3,4,5,6,7
};

__aligned(16) float cube_edge_crease_weights[12] = {
  inf, inf, inf, inf, inf, inf, inf, inf, inf, inf, inf, inf
};

__aligned(16) unsigned int cube_edge_crease_indices[24] =
{
  0,1, 1,2, 2,3, 3,0,
  4,5, 5,6, 6,7, 7,4,
  0,4, 1,5, 2,6, 3,7,
};

#define NUM_QUAD_INDICES 24
#define NUM_QUAD_FACES 6

unsigned int cube_quad_indices[24] = {
  0, 4, 5, 1,
  1, 5, 6, 2,
  2, 6, 7, 3,
  0, 3, 7, 4,
  4, 7, 6, 5,
  0, 1, 2, 3,
};

unsigned int cube_quad_faces[6] = {
  4, 4, 4, 4, 4, 4
};

#define NUM_TRI_INDICES 36
#define NUM_TRI_FACES 12

unsigned int cube_tri_indices[36] = {
  1, 4, 5,  0, 4, 1,
  2, 5, 6,  1, 5, 2,
  3, 6, 7,  2, 6, 3,
  4, 3, 7,  0, 3, 4,
  5, 7, 6,  4, 7, 5,
  3, 1, 2,  0, 1, 3
};

unsigned int cube_tri_faces[12] = {
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
};

#define NUM_HAIR_VERTICES 4

__aligned(16) float hair_vertices[4][4] =
{
  { 0.0f, 0.0f, 0.0f, 0.1f },
  { 0.5f, 1.0f, 0.0f, 0.1f },
  { 0.0f, 2.0f, -0.5f, 0.1f },
  { 0.0f, 3.0f, 0.0f, 0.1f }
};

__aligned(16) float hair_vertex_colors[4][4] =
{
  {  1.0f,  0.0f,  0.0f, 0.0f },
  {  1.0f,  1.0f,  0.0f, 0.0f },
  {  0.0f,  0.0f,  1.0f, 0.0f },
  {  1.0f,  1.0f,  1.0f, 0.0f },
};

unsigned int hair_indices[1] = {
  0
};

inline float updateEdgeLevel(const Vec3fa& cam_pos, Vec3fa* vtx, unsigned int* indices, const size_t e0, const size_t e1)
{
  const Vec3fa v0 = vtx[indices[e0]];
  const Vec3fa v1 = vtx[indices[e1]];
  const Vec3fa edge = v1-v0;
  const Vec3fa P = 0.5f*(v1+v0);
  const Vec3fa dist = cam_pos - P;
  const float level = max(min(LEVEL_FACTOR*(0.5f*length(edge)/length(dist)),MAX_EDGE_LEVEL),MIN_EDGE_LEVEL);
  return level;
}

/* adds a subdiv cube to the scene */
unsigned int addTriangleSubdivCube (RTCScene scene_i, const Vec3fa& pos)
{
  RTCGeometry geom = rtcNewSubdivisionMesh(g_device);

  //rtcSetBuffer(geom, RTC_VERTEX_BUFFER, cube_vertices, 0, sizeof(Vec3fa  ), NUM_VERTICES);
  Vec3fa* vtx = (Vec3fa*) rtcNewBuffer(geom, RTC_VERTEX_BUFFER,sizeof(Vec3fa),NUM_VERTICES);
  for (size_t i=0; i<NUM_VERTICES; i++) vtx[i] = Vec3fa(cube_vertices[i][0]+pos.x,cube_vertices[i][1]+pos.y,cube_vertices[i][2]+pos.z);

  rtcSetBuffer(geom, RTC_INDEX_BUFFER,  cube_tri_indices , 0, sizeof(unsigned int), NUM_TRI_INDICES);
  rtcSetBuffer(geom, RTC_FACE_BUFFER,   cube_tri_faces,    0, sizeof(unsigned int), NUM_TRI_FACES);

  rtcSetBuffer(geom, RTC_EDGE_CREASE_INDEX_BUFFER,   cube_edge_crease_indices,  0, 2*sizeof(unsigned int), 0);
  rtcSetBuffer(geom, RTC_EDGE_CREASE_WEIGHT_BUFFER,  cube_edge_crease_weights,  0, sizeof(float), 0);

  rtcSetBuffer(geom, RTC_VERTEX_CREASE_INDEX_BUFFER, cube_vertex_crease_indices,0, sizeof(unsigned int), 0);
  rtcSetBuffer(geom, RTC_VERTEX_CREASE_WEIGHT_BUFFER,cube_vertex_crease_weights,0, sizeof(float), 0);

  rtcSetBuffer(geom, RTC_USER_VERTEX_BUFFER0, cube_vertex_colors, 0, sizeof(Vec3fa), NUM_VERTICES);

  float* level = (float*) rtcNewBuffer(geom, RTC_LEVEL_BUFFER, sizeof(float), NUM_TRI_INDICES);
  for (size_t i=0; i<NUM_TRI_INDICES; i++) level[i] = FIXED_EDGE_TESSELLATION_VALUE;

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene_i, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

void setTriangleSubdivCubeLevels (RTCGeometry geom, const Vec3fa& cam_pos)
{
  Vec3fa* vtx = (Vec3fa*) rtcGetBuffer(geom, RTC_VERTEX_BUFFER);
  float* level = (float*) rtcGetBuffer(geom, RTC_LEVEL_BUFFER);

  for (size_t i=0; i<NUM_TRI_INDICES; i+=3)
  {
    level[i+0] = updateEdgeLevel(cam_pos, vtx, cube_tri_indices, i+0, i+1);
    level[i+1] = updateEdgeLevel(cam_pos, vtx, cube_tri_indices, i+1, i+2);
    level[i+2] = updateEdgeLevel(cam_pos, vtx, cube_tri_indices, i+2, i+0);
  }

  rtcUpdateBuffer(geom, RTC_LEVEL_BUFFER);
  rtcCommitGeometry(geom);
}

/* adds a subdiv cube to the scene */
unsigned int addQuadSubdivCube (RTCScene scene_i, const Vec3fa& pos)
{
  RTCGeometry geom = rtcNewSubdivisionMesh(g_device);

  //rtcSetBuffer(geom, RTC_VERTEX_BUFFER, cube_vertices, 0, sizeof(Vec3fa  ), NUM_VERTICES);
  Vec3fa* vtx = (Vec3fa*) rtcNewBuffer(geom, RTC_VERTEX_BUFFER,sizeof(Vec3fa),NUM_VERTICES);
  for (size_t i=0; i<NUM_VERTICES; i++) vtx[i] = Vec3fa(cube_vertices[i][0]+pos.x,cube_vertices[i][1]+pos.y,cube_vertices[i][2]+pos.z);

  rtcSetBuffer(geom, RTC_INDEX_BUFFER,  cube_quad_indices , 0, sizeof(unsigned int), NUM_QUAD_INDICES);
  rtcSetBuffer(geom, RTC_FACE_BUFFER,   cube_quad_faces,    0, sizeof(unsigned int), NUM_QUAD_FACES);

  rtcSetBuffer(geom, RTC_EDGE_CREASE_INDEX_BUFFER,   cube_edge_crease_indices,  0, 2*sizeof(unsigned int), 0);
  rtcSetBuffer(geom, RTC_EDGE_CREASE_WEIGHT_BUFFER,  cube_edge_crease_weights,  0, sizeof(float), 0);

  rtcSetBuffer(geom, RTC_VERTEX_CREASE_INDEX_BUFFER, cube_vertex_crease_indices,0, sizeof(unsigned int), 0);
  rtcSetBuffer(geom, RTC_VERTEX_CREASE_WEIGHT_BUFFER,cube_vertex_crease_weights,0, sizeof(float), 0);

  rtcSetBuffer(geom, RTC_USER_VERTEX_BUFFER0, cube_vertex_colors, 0, sizeof(Vec3fa), NUM_VERTICES);

  float* level = (float*) rtcNewBuffer(geom, RTC_LEVEL_BUFFER,sizeof(float),NUM_QUAD_INDICES);
  for (size_t i=0; i<NUM_QUAD_INDICES; i++) level[i] = FIXED_EDGE_TESSELLATION_VALUE;

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene_i, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

void setQuadSubdivCubeLevels (RTCGeometry geom, const Vec3fa& cam_pos)
{
  Vec3fa* vtx = (Vec3fa*) rtcGetBuffer(geom, RTC_VERTEX_BUFFER);
  float* level = (float*) rtcGetBuffer(geom, RTC_LEVEL_BUFFER);

  for (size_t i=0; i<NUM_QUAD_INDICES; i+=4)
  {
    level[i+0] = updateEdgeLevel(cam_pos, vtx, cube_quad_indices, i+0, i+1);
    level[i+1] = updateEdgeLevel(cam_pos, vtx, cube_quad_indices, i+1, i+2);
    level[i+2] = updateEdgeLevel(cam_pos, vtx, cube_quad_indices, i+2, i+3);
    level[i+3] = updateEdgeLevel(cam_pos, vtx, cube_quad_indices, i+3, i+0);
  }

  rtcUpdateBuffer(geom, RTC_LEVEL_BUFFER);
  rtcCommitGeometry(geom);
}

/* adds a triangle cube to the scene */
unsigned int addTriangleCube (RTCScene scene_i, const Vec3fa& pos)
{
  RTCGeometry geom = rtcNewTriangleMesh(g_device);

  //rtcSetBuffer(geom, RTC_VERTEX_BUFFER, cube_vertices, 0, sizeof(Vec3fa  ), NUM_VERTICES);
  Vec3fa* vtx = (Vec3fa*) rtcNewBuffer(geom, RTC_VERTEX_BUFFER,sizeof(Vec3fa),NUM_VERTICES);
  for (size_t i=0; i<NUM_VERTICES; i++) vtx[i] = Vec3fa(cube_vertices[i][0]+pos.x,cube_vertices[i][1]+pos.y,cube_vertices[i][2]+pos.z);

  rtcSetBuffer(geom, RTC_INDEX_BUFFER,  cube_tri_indices , 0, 3*sizeof(unsigned int), NUM_TRI_INDICES/3);
  rtcSetBuffer(geom, RTC_USER_VERTEX_BUFFER0, cube_vertex_colors, 0, sizeof(Vec3fa), NUM_VERTICES);

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene_i, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

/* adds a quad cube to the scene */
unsigned int addQuadCube (RTCScene scene_i, const Vec3fa& pos)
{
  RTCGeometry geom = rtcNewQuadMesh(g_device);

  //rtcSetBuffer(geom, RTC_VERTEX_BUFFER, cube_vertices, 0, sizeof(Vec3fa  ), NUM_VERTICES);
  Vec3fa* vtx = (Vec3fa*) rtcNewBuffer(geom, RTC_VERTEX_BUFFER,sizeof(Vec3fa),NUM_VERTICES);
  for (size_t i=0; i<NUM_VERTICES; i++) vtx[i] = Vec3fa(cube_vertices[i][0]+pos.x,cube_vertices[i][1]+pos.y,cube_vertices[i][2]+pos.z);

  rtcSetBuffer(geom, RTC_INDEX_BUFFER,  cube_quad_indices , 0, 4*sizeof(unsigned int), NUM_QUAD_INDICES/4);
  rtcSetBuffer(geom, RTC_USER_VERTEX_BUFFER0, cube_vertex_colors, 0, sizeof(Vec3fa), NUM_VERTICES);

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene_i, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

/* add curve geometry */
unsigned int addCurve (RTCScene scene, const Vec3fa& pos)
{
  RTCGeometry geom = rtcNewCurveGeometry (g_device, RTC_BASIS_BEZIER);
  rtcSetGeometryIntersector(geom,RTC_GEOMETRY_INTERSECTOR_SURFACE);

  //rtcSetBuffer(geom, RTC_VERTEX_BUFFER, hair_vertices, 0, sizeof(Vec3fa), NUM_HAIR_VERTICES);
  Vec3fa* vtx = (Vec3fa*) rtcNewBuffer(geom, RTC_VERTEX_BUFFER, sizeof(Vec3fa), NUM_HAIR_VERTICES);
  for (size_t i=0; i<NUM_HAIR_VERTICES; i++) {
    vtx[i].x = hair_vertices[i][0]+pos.x;
    vtx[i].y = hair_vertices[i][1]+pos.y;
    vtx[i].z = hair_vertices[i][2]+pos.z;
    vtx[i].w = hair_vertices[i][3];
  }

  rtcSetBuffer(geom, RTC_INDEX_BUFFER,  hair_indices , 0, sizeof(unsigned int), 1);
  rtcSetBuffer(geom, RTC_USER_VERTEX_BUFFER0, hair_vertex_colors, 0, sizeof(Vec3fa), NUM_HAIR_VERTICES);

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

/* adds a ground plane to the scene */
unsigned int addGroundPlane (RTCScene scene_i)
{
  /* create a triangulated plane with 2 triangles and 4 vertices */
  RTCGeometry geom = rtcNewTriangleMesh (g_device);

  /* set vertices */
  Vertex* vertices = (Vertex*) rtcNewBuffer(geom,RTC_VERTEX_BUFFER,sizeof(Vertex),4);
  vertices[0].x = -10; vertices[0].y = -2; vertices[0].z = -10;
  vertices[1].x = -10; vertices[1].y = -2; vertices[1].z = +10;
  vertices[2].x = +10; vertices[2].y = -2; vertices[2].z = -10;
  vertices[3].x = +10; vertices[3].y = -2; vertices[3].z = +10;

  /* set triangles */
  Triangle* triangles = (Triangle*) rtcNewBuffer(geom,RTC_INDEX_BUFFER,sizeof(Triangle),2);
  triangles[0].v0 = 0; triangles[0].v1 = 1; triangles[0].v2 = 2;
  triangles[1].v0 = 1; triangles[1].v1 = 3; triangles[1].v2 = 2;

  rtcCommitGeometry(geom);
  unsigned int geomID = rtcAttachGeometry(scene_i, geom);
  rtcReleaseGeometry(geom);
  return geomID;
}

/* called by the C++ code for initialization */
extern "C" void device_init (char* cfg)
{
  /* create new Embree device */
  g_device = rtcNewDevice(cfg);
  error_handler(nullptr,rtcDeviceGetError(g_device));

  /* set error handler */
  rtcDeviceSetErrorFunction(g_device,error_handler,nullptr);

  /* create scene */
  g_scene = rtcDeviceNewScene(g_device);

  /* add ground plane */
  addGroundPlane(g_scene);

  /* add cubes */
  addCurve(g_scene,Vec3fa(4.0f,-1.0f,-3.5f));
  quadCubeID = addQuadSubdivCube(g_scene,Vec3fa(4.0f,0.0f,0.0f));
  triCubeID  = addTriangleSubdivCube(g_scene,Vec3fa(4.0f,0.0f,3.5f));
  addTriangleCube(g_scene,Vec3fa(0.0f,0.0f,-3.0f));
  addQuadCube(g_scene,Vec3fa(0.0f,0.0f,3.0f));

  /* commit changes to scene */
  rtcCommit (g_scene);

  /* set start render mode */
  renderTile = renderTileStandard;
  key_pressed_handler = device_key_pressed_default;
}

/* task that renders a single screen tile */
Vec3fa renderPixelStandard(float x, float y, const ISPCCamera& camera, RayStats& stats)
{
  RTCIntersectContext context;
  rtcInitIntersectionContext(&context);
  
  /* initialize ray */
  Ray ray(Vec3fa(camera.xfm.p), Vec3fa(normalize(x*camera.xfm.l.vx + y*camera.xfm.l.vy + camera.xfm.l.vz)), 0.0f, inf);

  /* intersect ray with scene */
  rtcIntersect1(g_scene,&context,RTCRay_(ray));
  RayStats_addRay(stats);

  /* shade pixels */
  Vec3fa color = Vec3fa(0.0f);
  if (ray.geomID != RTC_INVALID_GEOMETRY_ID)
  {
    /* interpolate diffuse color */

    Vec3fa diffuse = Vec3fa(1.0f,0.0f,0.0f);
    if (ray.geomID > 0)
    {
      unsigned int geomID = ray.geomID; {
        rtcInterpolate(rtcGetGeometry(g_scene,geomID),ray.primID,ray.u,ray.v,RTC_USER_VERTEX_BUFFER0,&diffuse.x,nullptr,nullptr,nullptr,nullptr,nullptr,3);
      }
      //return diffuse;
      diffuse = 0.5f*diffuse;
    }

    /* calculate smooth shading normal */
    Vec3fa Ng = ray.Ng;
    if (ray.geomID == 2 || ray.geomID == 3) {
      Vec3fa dPdu,dPdv;
      unsigned int geomID = ray.geomID; {
        rtcInterpolate(rtcGetGeometry(g_scene,geomID),ray.primID,ray.u,ray.v,RTC_VERTEX_BUFFER0,nullptr,&dPdu.x,&dPdv.x,nullptr,nullptr,nullptr,3);
      }
      //return dPdu;
      Ng = cross(dPdu,dPdv);
    }
    Ng = normalize(Ng);
    color = color + diffuse*0.5f;
    Vec3fa lightDir = normalize(Vec3fa(-1,-1,-1));

    /* initialize shadow ray */
    Ray shadow(ray.org + ray.tfar()*ray.dir, neg(lightDir), 0.001f, inf);

    /* trace shadow ray */
    rtcOccluded1(g_scene,&context,RTCRay_(shadow));
    RayStats_addShadowRay(stats);

    /* add light contribution */
    if (shadow.geomID) {
      Vec3fa r = normalize(reflect(ray.dir,Ng));
      float s = pow(clamp(dot(r,lightDir),0.0f,1.0f),10.0f);
      float d = clamp(-dot(lightDir,Ng),0.0f,1.0f);
      color = color + diffuse*d + 0.5f*Vec3fa(s);
    }
  }
  return color;
}

/* renders a single screen tile */
void renderTileStandard(int taskIndex,
                        int threadIndex,
                        int* pixels,
                        const unsigned int width,
                        const unsigned int height,
                        const float time,
                        const ISPCCamera& camera,
                        const int numTilesX,
                        const int numTilesY)
{
  const unsigned int tileY = taskIndex / numTilesX;
  const unsigned int tileX = taskIndex - tileY * numTilesX;
  const unsigned int x0 = tileX * TILE_SIZE_X;
  const unsigned int x1 = min(x0+TILE_SIZE_X,width);
  const unsigned int y0 = tileY * TILE_SIZE_Y;
  const unsigned int y1 = min(y0+TILE_SIZE_Y,height);

  for (unsigned int y=y0; y<y1; y++) for (unsigned int x=x0; x<x1; x++)
  {
    /* calculate pixel color */
    Vec3fa color = renderPixelStandard((float)x,(float)y,camera,g_stats[threadIndex]);

    /* write color to framebuffer */
    unsigned int r = (unsigned int) (255.0f * clamp(color.x,0.0f,1.0f));
    unsigned int g = (unsigned int) (255.0f * clamp(color.y,0.0f,1.0f));
    unsigned int b = (unsigned int) (255.0f * clamp(color.z,0.0f,1.0f));
    pixels[y*width+x] = (b << 16) + (g << 8) + r;
  }
}

/* task that renders a single screen tile */
void renderTileTask (int taskIndex, int threadIndex, int* pixels,
                         const unsigned int width,
                         const unsigned int height,
                         const float time,
                         const ISPCCamera& camera,
                         const int numTilesX,
                         const int numTilesY)
{
  renderTile(taskIndex,threadIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
}

/* called by the C++ code to render */
extern "C" void device_render (int* pixels,
                           const unsigned int width,
                           const unsigned int height,
                           const float time,
                           const ISPCCamera& camera)
{
#if !defined(FORCE_FIXED_EDGE_TESSELLATION)
  setQuadSubdivCubeLevels (rtcGetGeometry(g_scene, quadCubeID), camera.xfm.p);
  setTriangleSubdivCubeLevels (rtcGetGeometry(g_scene, triCubeID), camera.xfm.p);
#endif

  rtcCommit(g_scene);

  const int numTilesX = (width +TILE_SIZE_X-1)/TILE_SIZE_X;
  const int numTilesY = (height+TILE_SIZE_Y-1)/TILE_SIZE_Y;
  parallel_for(size_t(0),size_t(numTilesX*numTilesY),[&](const range<size_t>& range) {
    const int threadIndex = (int)TaskScheduler::threadIndex();
    for (size_t i=range.begin(); i<range.end(); i++)
      renderTileTask((int)i,threadIndex,pixels,width,height,time,camera,numTilesX,numTilesY);
  }); 
}

/* called by the C++ code for cleanup */
extern "C" void device_cleanup ()
{
  rtcReleaseScene (g_scene); g_scene = nullptr;
  rtcReleaseDevice(g_device); g_device = nullptr;
}

} // namespace embree
