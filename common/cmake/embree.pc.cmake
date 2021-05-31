prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${exec_prefix}/@LIB_INSTALL_DIR@
includedir=${prefix}/include

Name: embree
Description: Embree ray tracing kernels
Version: @EMBREE_VERSION@

Libs: -L${libdir} -lembree3
Cflags: -I${includedir}
