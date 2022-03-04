MESSAGE(STATUS "Building for macOS")

SET(MACOS 1)

IF (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    MESSAGE(STATUS "Building for Apple silicon")
    SET(EMBREE_ARM ON)
    SET(EMBREE_NEON_AVX2_EMULATION ON)
    SET(EMBREE_ISPC_SUPPORT         OFF     CACHE   BOOL    "" FORCE)
ENDIF()

SET(EMBREE_TUTORIALS            ON      CACHE   BOOL    "")
# SET(EMBREE_TUTORIALS_GLFW       ON      CACHE   BOOL    "")
# SET(EMBREE_TUTORIALS_LIBPNG     ON      CACHE   BOOL    "")
# SET(EMBREE_TUTORIALS_LIBJPEG    ON      CACHE   BOOL    "")
SET(EMBREE_RAY_PACKETS          ON      CACHE   BOOL    "")
