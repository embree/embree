## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

INCLUDE(GNUInstallDirs)

IF (NOT EMBREE_ZIP_MODE AND NOT WIN32 AND NOT APPLE)
  SET(CMAKE_INSTALL_BINDIR ${CMAKE_INSTALL_BINDIR}/embree${EMBREE_VERSION_MAJOR})
  SET(CMAKE_INSTALL_FULL_BINDIR ${CMAKE_INSTALL_FULL_BINDIR}/embree${EMBREE_VERSION_MAJOR})
ENDIF()

# use full absolute path as install name
IF (NOT EMBREE_ZIP_MODE)
  SET(CMAKE_INSTALL_NAME_DIR ${CMAKE_INSTALL_FULL_LIBDIR})
  SET(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
ELSE()
  IF(APPLE)
    SET(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  ELSE()
    SET(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  ENDIF()
ENDIF()

##############################################################
# Install MSVC runtime
##############################################################

IF (WIN32)
  SET(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  INCLUDE(InstallRequiredSystemLibraries)
  LIST(FILTER CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS INCLUDE REGEX ".*msvcp[0-9]+\.dll|.*vcruntime[0-9]+\.dll|.*vcruntime[0-9]+_[0-9]+\.dll")
  INSTALL(FILES ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT lib)
ENDIF()

##############################################################
# Install Headers
##############################################################

INSTALL(DIRECTORY include/embree3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT devel)
IF (NOT WIN32)
  INSTALL(DIRECTORY man/man3 DESTINATION ${CMAKE_INSTALL_MANDIR} COMPONENT devel)
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

INSTALL(FILES ${PROJECT_SOURCE_DIR}/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/CHANGELOG.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/readme.pdf DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/third-party-programs.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)
INSTALL(FILES ${PROJECT_SOURCE_DIR}/third-party-programs-TBB.txt DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT lib)

##############################################################
# Install scripts to set embree paths
##############################################################

IF (EMBREE_ZIP_MODE)
  IF (WIN32)
  ELSEIF(APPLE)
    CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/scripts/install_macosx/embree-vars.sh embree-vars.sh @ONLY)
    CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/scripts/install_macosx/embree-vars.csh embree-vars.csh @ONLY)
    INSTALL(FILES ${PROJECT_BINARY_DIR}/embree-vars.sh DESTINATION "." COMPONENT lib)
    INSTALL(FILES ${PROJECT_BINARY_DIR}/embree-vars.csh DESTINATION "." COMPONENT lib)
  ELSE()
    CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/scripts/install_linux/embree-vars.sh embree-vars.sh @ONLY)
    CONFIGURE_FILE(${PROJECT_SOURCE_DIR}/scripts/install_linux/embree-vars.csh embree-vars.csh @ONLY)
    INSTALL(FILES ${PROJECT_BINARY_DIR}/embree-vars.sh DESTINATION "." COMPONENT lib)
    INSTALL(FILES ${PROJECT_BINARY_DIR}/embree-vars.csh DESTINATION "." COMPONENT lib)
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

IF (APPLE AND NOT EMBREE_ZIP_MODE)
  CONFIGURE_FILE(scripts/install_macosx/uninstall.command uninstall.command @ONLY)
  INSTALL(PROGRAMS "${PROJECT_BINARY_DIR}/uninstall.command" DESTINATION ${CMAKE_INSTALL_BINDIR}/.. COMPONENT lib)
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

INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-config-install.cmake" DESTINATION ${EMBREE_CMAKECONFIG_DIR} RENAME "embree-config.cmake" COMPONENT devel)
INSTALL(FILES "${PROJECT_BINARY_DIR}/embree-config-version.cmake" DESTINATION ${EMBREE_CMAKECONFIG_DIR} COMPONENT devel)

##############################################################
# Generate and Install Embree Pkgconfig
##############################################################

configure_file (common/cmake/embree.pc.cmake ${PROJECT_BINARY_DIR}//embree.pc @ONLY)
install (FILES ${PROJECT_BINARY_DIR}/embree.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

##############################################################
# CPack specific stuff
##############################################################

SET(CPACK_PACKAGE_NAME "Intel(R) Embree Ray Tracing Kernels")
SET(CPACK_PACKAGE_FILE_NAME "embree-${EMBREE_VERSION}")
#SET(CPACK_PACKAGE_ICON ${PROJECT_SOURCE_DIR}/embree-doc/images/icon.png)
#SET(CPACK_PACKAGE_RELOCATABLE TRUE)
SET(CPACK_STRIP_FILES TRUE)

SET(CPACK_PACKAGE_VERSION_MAJOR ${EMBREE_VERSION_MAJOR})
SET(CPACK_PACKAGE_VERSION_MINOR ${EMBREE_VERSION_MINOR})
SET(CPACK_PACKAGE_VERSION_PATCH ${EMBREE_VERSION_PATCH})
SET(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Intel(R) Embree implements high performance ray tracing kernels including accelertion structure construction and traversal.")
SET(CPACK_PACKAGE_VENDOR "Intel Corporation")
SET(CPACK_PACKAGE_CONTACT embree_support@intel.com)

SET(CPACK_COMPONENT_LIB_DISPLAY_NAME "Library")
SET(CPACK_COMPONENT_LIB_DESCRIPTION "The Embree library including documentation.")

SET(CPACK_COMPONENT_DEVEL_DISPLAY_NAME "Development")
SET(CPACK_COMPONENT_DEVEL_DESCRIPTION "Header Files for C and ISPC required to develop applications with Embree.")

SET(CPACK_COMPONENT_EXAMPLES_DISPLAY_NAME "Examples")
SET(CPACK_COMPONENT_EXAMPLES_DESCRIPTION "Tutorials demonstrating how to use Embree.")

# dependencies between components
#SET(CPACK_COMPONENT_DEVEL_DEPENDS lib)
#SET(CPACK_COMPONENT_EXAMPLES_DEPENDS lib)
#SET(CPACK_COMPONENT_LIB_REQUIRED ON) # always install the libs

# point to readme and license files
SET(CPACK_RESOURCE_FILE_README ${PROJECT_SOURCE_DIR}/README.md)
SET(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE.txt)

# Windows specific settings
IF(WIN32)

  IF (CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(ARCH x64)
    SET(CPACK_PACKAGE_NAME "${CPACK_PACKAGE_NAME} x64")
  ELSE()
    SET(ARCH win32)
    SET(CPACK_PACKAGE_NAME "${CPACK_PACKAGE_NAME} Win32")
  ENDIF()

  IF (MSVC12)
    SET(VCVER vc12)
  ELSEIF(MSVC14) # also for VC15, which is toolset v141
    SET(VCVER vc14)
  ENDIF()

  IF (NOT EMBREE_ZIP_MODE)
    SET(CPACK_GENERATOR WIX)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.${ARCH}.${VCVER}")
    SET(CPACK_PACKAGE_INSTALL_DIRECTORY "Intel\\\\Embree${EMBREE_VERSION_MAJOR}")
    SET(CPACK_WIX_PRODUCT_GUID "331BD5A9-DAD6-486B-A435-18AAB80${EMBREE_VERSION_NUMBER}")
    SET(CPACK_WIX_UPGRADE_GUID "331BD5A9-DAD6-486B-A435-18AAB80${EMBREE_VERSION_MAJOR}0000") # upgrade as long as major version is the same
    SET(CPACK_WIX_CMAKE_PACKAGE_REGISTRY TRUE)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_win.bat ${CMAKE_BUILD_TYPE} ${CPACK_PACKAGE_FILE_NAME}.msi ${EMBREE_SIGN_FILE})
    ENDIF()
  ELSE()
    SET(CPACK_GENERATOR ZIP)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.${ARCH}.${VCVER}.windows")
    SET(CPACK_MONOLITHIC_INSTALL 1)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_win.bat ${CMAKE_BUILD_TYPE} ${CPACK_PACKAGE_FILE_NAME}.zip)
    ENDIF()
  ENDIF()

# MacOSX specific settings
ELSEIF(APPLE)

  CONFIGURE_FILE(README.md README.txt)
  SET(CPACK_RESOURCE_FILE_README ${PROJECT_BINARY_DIR}/README.txt)

  IF (NOT EMBREE_ZIP_MODE)
    SET(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
    SET(CPACK_GENERATOR productbuild)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.x86_64")
    SET(CPACK_COMPONENTS_ALL lib devel examples)
    #SET(CPACK_MONOLITHIC_INSTALL 1)
    SET(CPACK_PACKAGE_NAME embree-${EMBREE_VERSION})
    SET(CPACK_PACKAGE_VENDOR "intel") # creates short name com.intel.embree3.xxx in pkgutil
    SET(CPACK_OSX_PACKAGE_VERSION 10.7)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_macosx.sh ${CMAKE_BUILD_TYPE} ${CPACK_PACKAGE_FILE_NAME}.pkg ${EMBREE_SIGN_FILE})
    ENDIF()
  ELSE()
    SET(CPACK_GENERATOR ZIP)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.x86_64.macosx")
    SET(CPACK_MONOLITHIC_INSTALL 1)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_macosx.sh ${CMAKE_BUILD_TYPE} ${CPACK_PACKAGE_FILE_NAME}.zip ${EMBREE_SIGN_FILE})
    ENDIF()
  ENDIF()

# Linux specific settings
ELSE()

  IF (NOT EMBREE_ZIP_MODE)

    SET(CPACK_GENERATOR RPM)
    SET(CPACK_RPM_PACKAGE_NAME "embree${EMBREE_VERSION_MAJOR}")
    SET(CPACK_COMPONENTS_ALL devel lib examples)
    SET(CPACK_RPM_COMPONENT_INSTALL ON)
    SET(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    SET(CPACK_RPM_devel_PACKAGE_ARCHITECTURE noarch)
    SET(CPACK_RPM_PACKAGE_LICENSE "ASL 2.0") # Apache Software License, Version 2.0
    SET(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
    SET(CPACK_RPM_CHANGELOG_FILE ${CMAKE_BINARY_DIR}/rpm_changelog.txt) # ChangeLog of the RPM
    IF (CMAKE_VERSION VERSION_LESS "3.7.0")
      EXECUTE_PROCESS(COMMAND date "+%a %b %d %Y" OUTPUT_VARIABLE CHANGELOG_DATE OUTPUT_STRIP_TRAILING_WHITESPACE)
    ELSE()
      STRING(TIMESTAMP CHANGELOG_DATE "%a %b %d %Y")
    ENDIF()
    SET(RPM_CHANGELOG "* ${CHANGELOG_DATE} Johannes Günther <johannes.guenther@intel.com> - ${EMBREE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}\n- First package")
    FILE(WRITE ${CPACK_RPM_CHANGELOG_FILE} ${RPM_CHANGELOG})
    SET(CPACK_RPM_PACKAGE_URL http://embree.github.io/)
    SET(CPACK_RPM_DEFAULT_DIR_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE) # otherwise cmake directory has wrong 775 permissions

    # post install and uninstall scripts
    SET(CPACK_RPM_lib_POST_INSTALL_SCRIPT_FILE ${PROJECT_SOURCE_DIR}/common/cmake/rpm_ldconfig.sh)
    SET(CPACK_RPM_lib_POST_UNINSTALL_SCRIPT_FILE ${PROJECT_SOURCE_DIR}/common/cmake/rpm_ldconfig.sh)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_linux.sh
        ${EMBREE_ZIP_MODE} ${EMBREE_LIBRARY_NAME} ${EMBREE_VERSION} ${EMBREE_VERSION_MAJOR} ${EMBREE_SIGN_FILE})
    ENDIF()
  ELSE()
  
    SET(CPACK_GENERATOR TGZ)
    SET(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.x86_64.linux")
    SET(CPACK_MONOLITHIC_INSTALL 1)
    IF (EMBREE_TESTING_PACKAGE)
      ADD_TEST(NAME "BuildPackage" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR} COMMAND ${PROJECT_SOURCE_DIR}/scripts/package_linux.sh
        ${EMBREE_ZIP_MODE} ${EMBREE_LIBRARY_NAME} ${EMBREE_VERSION} ${EMBREE_VERSION_MAJOR} ${EMBREE_SIGN_FILE})
    ENDIF()
  ENDIF()
  
ENDIF()

IF (EMBREE_TESTING_PACKAGE)
  SET_TESTS_PROPERTIES(BuildPackage PROPERTIES TIMEOUT 1200)
ENDIF()
