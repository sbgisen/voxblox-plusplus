cmake_minimum_required(VERSION 2.8.3)
project(global_segment_map)

add_definitions(-std=c++14)
if (NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  add_definitions(-fext-numeric-literals)
endif()

find_package(catkin_simple REQUIRED)
find_package(Boost REQUIRED COMPONENTS filesystem system thread)
find_package(PCL REQUIRED)
catkin_simple(ALL_DEPS_REQUIRED)

cs_add_library(${PROJECT_NAME}
  src/label_block_serialization.cc
  src/semantic_instance_label_fusion.cc
  src/label_merge_integrator.cc
  src/icp_utils.cc
  src/label_tsdf_integrator.cc
  src/label_tsdf_map.cc
  src/meshing/label_tsdf_mesh_integrator.cc
  src/meshing/label_color_map.cc
  src/meshing/instance_color_map.cc
  src/meshing/semantic_color_map.cc
  src/segment.cc
  src/utils/visualizer.cc
)
target_link_libraries(${PROJECT_NAME} ${PCL_LIBRARIES})
target_include_directories(${PROJECT_NAME} PRIVATE ${PCL_INCLUDE_DIRS})

cs_install()
cs_export()
