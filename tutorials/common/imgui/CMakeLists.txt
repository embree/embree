## Copyright 2009-2021 Intel Corporation
## SPDX-License-Identifier: Apache-2.0

ADD_LIBRARY(imgui STATIC
  imgui.cpp
  imgui_draw.cpp
  imgui_widgets.cpp
  imgui_tables.cpp
  backends/imgui_impl_glfw.cpp
  backends/imgui_impl_opengl2.cpp
)

TARGET_INCLUDE_DIRECTORIES(imgui PUBLIC . ./backends)
TARGET_LINK_LIBRARIES(imgui glfw)
SET_PROPERTY(TARGET imgui PROPERTY FOLDER tutorials/common)
SET_PROPERTY(TARGET imgui APPEND PROPERTY COMPILE_FLAGS " ${FLAGS_LOWEST}")

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin" OR ${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm64|aarch64")
  TARGET_COMPILE_DEFINITIONS(imgui PUBLIC IMGUI_DISABLE_SSE)
endif()
