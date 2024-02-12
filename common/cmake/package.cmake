## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

INCLUDE(GNUInstallDirs)

IF (NOT EMBREE_ZIP_MODE AND NOT WIN32 AND NOT APPLE)
  SET(CMAKE_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}/embree${EMBREE_VERSION_MAJOR}")
  SET(CMAKE_INSTALL_FULL_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}/embree${EMBREE_VERSION_MAJOR}")
ENDIF()

# use full absolute path as install name
IF (NOT EMBREE_ZIP_MODE)
  SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
ELSE()
  IF(APPLE)
    SET(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  ELSE()
    SET(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  ENDIF()
ENDIF()

##############################################################
# Install SYCL specific files
##############################################################

# SYCL library
IF (EMBREE_SYCL_SUPPORT AND EMBREE_INSTALL_DEPENDENCIES)
  GET_FILENAME_COMPONENT(DPCPP_COMPILER_DIR ${CMAKE_CXX_COMPILER} PATH)
  
  IF (WIN32)
    
    FILE(GLOB LIB_SYCL_LIB_FILES LIST_DIRECTORIES FALSE
      "${DPCPP_COMPILER_DIR}/../lib/sycl.lib"
      "${DPCPP_COMPILER_DIR}/../lib/sycl?.lib")
    INSTALL(FILES ${LIB_SYCL_LIB_FILES} DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)

    FILE(GLOB LIB_SYCL_DLL_FILES LIST_DIRECTORIES FALSE
      "${DPCPP_COMPILER_DIR}/../bin/sycl.dll"
      "${DPCPP_COMPILER_DIR}/../bin/sycl?.dll")
    INSTALL(FILES ${LIB_SYCL_DLL_FILES} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    
    INSTALL(FILES "${DPCPP_COMPILER_DIR}/pi_level_zero.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    INSTALL(FILES "${DPCPP_COMPILER_DIR}/win_proxy_loader.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib OPTIONAL)

    IF (EXISTS "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/svml_dispmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/svml_dispmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/libmmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/libmmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/libmmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/libmmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()

    IF (EXISTS "${DPCPP_COMPILER_DIR}/pi_win_proxy_loader.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/pi_win_proxy_loader.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
  ELSE()
    
    FILE(GLOB LIB_SYCL_FILES LIST_DIRECTORIES FALSE
      "${DPCPP_COMPILER_DIR}/../lib/libsycl.so"
      "${DPCPP_COMPILER_DIR}/../lib/libsycl.so.?"
      "${DPCPP_COMPILER_DIR}/../lib/libsycl.so.?.?"
      "${DPCPP_COMPILER_DIR}/../lib/libsycl.so.?.?.?"
      "${DPCPP_COMPILER_DIR}/../lib/libsycl.so.?.?.?-?")
    INSTALL(FILES ${LIB_SYCL_FILES} DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    
    INSTALL(FILES "${DPCPP_COMPILER_DIR}/../lib/libpi_level_zero.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)

    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libsvml.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libsvml.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libirng.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libirng.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libimf.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libimf.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so")
      FILE(GLOB LIB_SYCL_FILES LIST_DIRECTORIES FALSE
        "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so"
        "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so.?")
      INSTALL(FILES ${LIB_SYCL_FILES} DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()
   
  ENDIF()
ENDIF()

##############################################################
# Install MSVC runtime
##############################################################

IF (WIN32)
  IF(SYCL_ONEAPI_ICX AND EMBREE_INSTALL_DEPENDENCIES)
    GET_FILENAME_COMPONENT(DPCPP_COMPILER_DIR ${CMAKE_CXX_COMPILER} PATH)
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/libmmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/libmmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}//libmmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/libmmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/svml_dispmd.dll")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../redist/intel64_win/compiler/svml_dispmd.dll" DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)
    ENDIF()
  ENDIF()

  SET(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  SET(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
  INCLUDE(InstallRequiredSystemLibraries)
  LIST(FILTER CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS INCLUDE REGEX ".*msvcp[0-9]+\.dll|.*vcruntime[0-9]+\.dll|.*vcruntime[0-9]+_[0-9]+\.dll")
  INSTALL(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT lib)

ELSE()

  IF(SYCL_ONEAPI_ICX AND EMBREE_INSTALL_DEPENDENCIES)

    GET_FILENAME_COMPONENT(DPCPP_COMPILER_DIR ${CMAKE_CXX_COMPILER} PATH)
    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libsvml.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libsvml.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()

    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libirng.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libirng.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()

    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libimf.so")
      INSTALL(FILES "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libimf.so" DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()

    IF (EXISTS "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so")
      FILE(GLOB LIB_SYCL_FILES LIST_DIRECTORIES FALSE
        "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so"
        "${DPCPP_COMPILER_DIR}/../compiler/lib/intel64_lin/libintlc.so.?")
      INSTALL(FILES ${LIB_SYCL_FILES} DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT lib)
    ENDIF()

  ENDIF()

ENDIF()

##############################################################
# Install Headers
##############################################################

INSTALL(DIRECTORY include/embree4 DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT devel)
IF (NOT WIN32)
  INSTALL(DIRECTORY man/man3 DESTINATION "${CMAKE_INSTALL_MANDIR}" COMPONENT devel)
ENDIF()

##############################################################
# Install Models
##############################################################

IF (EMBREE_TUTORIALS)
  INSTALL(DIRECTORY tutorials/models DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT examples)
ENDIF()

##############################################################
# Install Documentation
##############################################################

INSTALL(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/CHANGELOG.md" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/readme.pdf" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/third-party-programs.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/third-party-programs-TBB.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/third-party-programs-OIDN.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/third-party-programs-DPCPP.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)
INSTALL(FILES "${PROJECT_SOURCE_DIR}/third-party-programs-oneAPI-DPCPP.txt" DESTINATION "${CMAKE_INSTALL_DOCDIR}" COMPONENT lib)

##############################################################
# Install scripts to set embree paths
##############################################################

IF (EMBREE_ZIP_MODE)
  IF (WIN32)
  ELSEIF(APPLE)
    CONFIGURE_FILE("${PROJECT_SOURCE_DIR}/scripts/install_macosx/embree-vars.sh" embree-vars.sh @ONLY)
    CONFIGURE_FILE("${PROJECT_SOURCE_DIR}/scripts/install_macosx/embree-vars.csh" embree-vars.csh @ONLY)
    INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-vars.sh" DESTINATION "." COMPONENT lib)
    INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-vars.csh" DESTINATION "." COMPONENT lib)
  ELSE()
    CONFIGURE_FILE("${PROJECT_SOURCE_DIR}/scripts/install_linux/embree-vars.sh" embree-vars.sh @ONLY)
    CONFIGURE_FILE("${PROJECT_SOURCE_DIR}/scripts/install_linux/embree-vars.csh" embree-vars.csh @ONLY)
    INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-vars.sh" DESTINATION "." COMPONENT lib)
    INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-vars.csh" DESTINATION "." COMPONENT lib)
  ENDIF()
ENDIF()

##############################################################
# Install Embree CMake Configuration
##############################################################

IF (NOT EMBREE_ZIP_MODE)
  SET(EMBREE_CONFIG_VERSION ${EMBREE_VERSION})
ELSE()
  SET(EMBREE_CONFIG_VERSION ${EMBREE_VERSION_MAJOR})
ENDIF()

# why does this have to be so complicated...
IF (EMBREE_STATIC_LIB)
  SET(EMBREE_LIBRARY_FULLNAME ${CMAKE_STATIC_LIBRARY_PREFIX}${EMBREE_LIBRARY_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})
ELSE()
  IF (WIN32)
    SET(EMBREE_LIBRARY_FULLNAME ${CMAKE_IMPORT_LIBRARY_PREFIX}${EMBREE_LIBRARY_NAME}${CMAKE_IMPORT_LIBRARY_SUFFIX})
  ELSEIF (APPLE)
    SET(EMBREE_LIBRARY_FULLNAME ${CMAKE_SHARED_LIBRARY_PREFIX}${EMBREE_LIBRARY_NAME}.${EMBREE_CONFIG_VERSION}${CMAKE_SHARED_LIBRARY_SUFFIX})
  ELSE()
    SET(EMBREE_LIBRARY_FULLNAME ${CMAKE_SHARED_LIBRARY_PREFIX}${EMBREE_LIBRARY_NAME}${CMAKE_SHARED_LIBRARY_SUFFIX}.${EMBREE_CONFIG_VERSION})
  ENDIF()
ENDIF()

#IF (WIN32 OR EMBREE_ZIP_MODE)
  # for local "installs" and on Windows we want the cmake config files placed
  # in the install root, such that users can point the CMake variable
  # embree_DIR just to the install folder
#  SET(EMBREE_CMAKECONFIG_DIR ".")
#  SET(EMBREE_CMAKEEXPORT_DIR "cmake")
#  SET(EMBREE_RELATIVE_ROOT_DIR ".")
#ELSE()
SET(EMBREE_CMAKECONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/embree-${EMBREE_VERSION}")
SET(EMBREE_CMAKEEXPORT_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/embree-${EMBREE_VERSION}")
IF (WIN32)
  SET(EMBREE_RELATIVE_ROOT_DIR "../../../")
ELSE()
  FILE(RELATIVE_PATH EMBREE_RELATIVE_ROOT_DIR "/${EMBREE_CMAKECONFIG_DIR}" "/")
ENDIF()
#ENDIF()

CONFIGURE_FILE(common/cmake/embree-config.cmake embree-config-install.cmake @ONLY)
CONFIGURE_FILE(common/cmake/embree-config-version.cmake embree-config-version.cmake @ONLY)
# create a config file for the build directory
CONFIGURE_FILE(common/cmake/embree-config-builddir.cmake embree-config.cmake @ONLY)

INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-config-install.cmake" DESTINATION "${EMBREE_CMAKECONFIG_DIR}" RENAME "embree-config.cmake" COMPONENT devel)
INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-config-version.cmake" DESTINATION "${EMBREE_CMAKECONFIG_DIR}" COMPONENT devel)

##############################################################
# Generate and Install Embree Pkgconfig
##############################################################

configure_file (common/cmake/embree.pc.cmake ${PROJECT_BINARY_DIR}//embree.pc @ONLY)
install (FILES ${PROJECT_BINARY_DIR}/embree.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

##############################################################
# CPack specific stuff
##############################################################

SET(CPACK_PACKAGE_NAME "Intel(R) Embree Ray Tracing Kernels")
SET(CPACK_PACKAGE_FILE_NAME "embree-${EMBREE_VERSION}${EMBREE_VERSION_NOTE}")
IF (EMBREE_SYCL_SUPPORT)
  SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.sycl")
  SET(EMBREE_VERSION_SUFFIX)
ENDIF()
#SET(CPACK_PACKAGE_ICON "${PROJECT_SOURCE_DIR}/embree-doc/images/icon.png")
#SET(CPACK_PACKAGE_RELOCATABLE TRUE)
SET(CPACK_STRIP_FILES TRUE)

SET(CPACK_PACKAGE_VERSION_MAJOR ${EMBREE_VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${EMBREE_VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${EMBREE_VERSION_PATCH})
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Intel(R) Embree implements high performance ray tracing kernels including acceleration structure construction and traversal.")
SET(CPACK_PACKAGE_VENDOR "Intel Corporation")
SET(CPACK_PACKAGE_CONTACT embree_support@intel.com)


SET(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
SET(CPACK_COMPONENTS_GROUPING ONE_PER_GROUP)

SET(CPACK_COMPONENT_LIB_DISPLAY_NAME "Library")
SET(CPACK_COMPONENT_LIB_DESCRIPTION "The Embree library including documentation.")
SET(CPACK_COMPONENT_LIB_GROUP "embree")

SET(CPACK_COMPONENT_DEVEL_DISPLAY_NAME "Development")
SET(CPACK_COMPONENT_DEVEL_DESCRIPTION "Header Files for C and ISPC required to develop applications with Embree.")
SET(CPACK_COMPONENT_DEVEL_GROUP "embree")

SET(CPACK_COMPONENT_EXAMPLES_DISPLAY_NAME "Examples")
SET(CPACK_COMPONENT_EXAMPLES_DESCRIPTION "Tutorials demonstrating how to use Embree.")
SET(CPACK_COMPONENT_EXAMPLES_GROUP "embree")

SET(CPACK_COMPONENT_TESTING_DISPLAY_NAME "Testing")
SET(CPACK_COMPONENT_TESTING_DESCRIPTION "Models and reference images for tests")
SET(CPACK_COMPONENT_TESTING_GROUP "embree-testing")

# dependencies between components
#SET(CPACK_COMPONENT_DEVEL_DEPENDS lib)
#SET(CPACK_COMPONENT_EXAMPLES_DEPENDS lib)
#SET(CPACK_COMPONENT_LIB_REQUIRED ON) # always install the libs

# point to readme and license files
SET(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
SET(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.txt")

# Windows specific settings
IF(WIN32)
  IF (CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(ARCH x64)
    SET(CPACK_PACKAGE_NAME "${CPACK_PACKAGE_NAME} x64")
  ELSE()
    SET(ARCH win32)
    SET(CPACK_PACKAGE_NAME "${CPACK_PACKAGE_NAME} Win32")
  ENDIF()

  SET(CPACK_GENERATOR ZIP)
  SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.${ARCH}.windows")
  SET(PACKAGE_BASE_NAME "${CPACK_PACKAGE_FILE_NAME}")
  SET(PACKAGE_EXT "zip")

# MacOSX specific settings
ELSEIF(APPLE)

  CONFIGURE_FILE(README.md README.txt)
  SET(CPACK_RESOURCE_FILE_README "${PROJECT_BINARY_DIR}/README.txt")

  SET(CPACK_GENERATOR ZIP)
  IF (EMBREE_ARM)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.arm64.macosx")
  ELSE()
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.x86_64.macosx")
  ENDIF()
  SET(PACKAGE_BASE_NAME "${CPACK_PACKAGE_FILE_NAME}")
  SET(PACKAGE_EXT "zip")

  add_custom_target(
    post_package_notarize "${PROJECT_SOURCE_DIR}/scripts/package_post_build_notarize_macosx.sh" ${PACKAGE_BASE_NAME} ${EMBREE_SIGN_FILE}
  )

# Linux specific settings
ELSE()
  SET(CPACK_GENERATOR TGZ)
  SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.x86_64.linux")
  SET(PACKAGE_BASE_NAME "${CPACK_PACKAGE_FILE_NAME}")
  SET(PACKAGE_EXT "tar.gz")
  IF (EMBREE_SYCL_SUPPORT)
    SET(EMBREE_VERSION_SYCL_SUFFIX ".sycl")
  ENDIF()
ENDIF()


add_custom_target(
  build ${CMAKE_COMMAND} --build . --config ${CMAKE_BUILD_TYPE} --target package -j8
  COMMAND ${CMAKE_COMMAND} -DPACKAGE_BASENAME=${PACKAGE_BASE_NAME} -DPACKAGE_EXT=${PACKAGE_EXT} -P ${PROJECT_SOURCE_DIR}/scripts/package_build.cmake
)

add_custom_target(
  test_package ${CMAKE_COMMAND} -DWHAT="UNPACK" -DPACKAGE_BASENAME=${PACKAGE_BASE_NAME} -DPACKAGE_EXT=${PACKAGE_EXT} -P ${PROJECT_SOURCE_DIR}/scripts/package_test.cmake
  COMMAND cd embree_install/testing && ${CMAKE_COMMAND} -B build -DEMBREE_TESTING_INTENSITY=${EMBREE_TESTING_INTENSITY}
  COMMAND ctest --test-dir ${CMAKE_CURRENT_BINARY_DIR}/embree_install/testing/build -VV -C ${CMAKE_BUILD_TYPE} --output-log ctest.output 
  COMMAND ${CMAKE_COMMAND} -DWHAT="CHECK" -P ${PROJECT_SOURCE_DIR}/scripts/package_test.cmake
)


if(WIN32)
  set(INTEGRATE_BINARY "./build/Release/test.exe")
else()
  set(INTEGRATE_BINARY "./build/test")
endif()

add_custom_target(
  test_integration ${CMAKE_COMMAND} -DWHAT="UNPACK" -DPACKAGE_BASENAME=${PACKAGE_BASE_NAME} -DPACKAGE_EXT=${PACKAGE_EXT} -P ${PROJECT_SOURCE_DIR}/scripts/package_test.cmake 
  COMMAND cd ${PROJECT_SOURCE_DIR}/tests/integration/test_embree_release && ${CMAKE_COMMAND} -B build --preset ${EMBREE_TESTING_INTEGRATION_PRESET} -Dembree_DIR="${CMAKE_CURRENT_BINARY_DIR}/embree_install/lib/cmake/embree-${EMBREE_VERSION}"
  COMMAND cd ${PROJECT_SOURCE_DIR}/tests/integration/test_embree_release && ${CMAKE_COMMAND} --build build --config Release
  COMMAND cd ${PROJECT_SOURCE_DIR}/tests/integration/test_embree_release && ${INTEGRATE_BINARY}
)