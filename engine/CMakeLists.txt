include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.1
  GIT_SHALLOW TRUE
)

find_package(SDL2 CONFIG QUIET)

if(NOT SDL2_FOUND)
  FetchContent_Declare(
    sdl2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.9
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(sdl2)
  set(BF2_SDL_TARGET SDL2::SDL2)
else()
  set(BF2_SDL_TARGET SDL2::SDL2)
endif()

FetchContent_Declare(
  enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG v1.3.18
  GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glm imgui enet)

add_library(bf2glew STATIC ${CMAKE_SOURCE_DIR}/third_party/glew/glew.c)
target_include_directories(bf2glew PUBLIC ${CMAKE_SOURCE_DIR}/third_party/glew)
target_compile_definitions(bf2glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
target_link_libraries(bf2glew PUBLIC opengl32)

add_library(bf2miniz STATIC ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(bf2miniz PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniz)

add_library(bf2engine STATIC
  formats/mesh/bf2_mesh.cpp
  formats/archive/archive.cpp
  formats/dds/dds_loader.cpp
  formats/animation/bf2_animation.cpp
  formats/collision/bf2_collision.cpp
  formats/effects/effect_bundle.cpp
  formats/nav/bf2_nav_mesh.cpp
  formats/terrain/terrain_loader.cpp
  formats/terrain/terrain_colormap.cpp
  formats/terrain/terrain_con_parser.cpp
  formats/terrain/heightmap_cluster.cpp
  anim/pose.cpp
  anim/skinning.cpp
  core/resource_manager.cpp
  core/scene_graph.cpp
  core/level_loader.cpp
  core/static_object_parser.cpp
  core/overgrowth_parser.cpp
  core/overgrowth_instances.cpp
  core/compiled_roads_parser.cpp
  core/asset_audit.cpp
  core/level_validator.cpp
  core/mesh_bounds.cpp
  core/placement_audit.cpp
  core/template_resolver.cpp
  render/renderer.cpp
  render/texture_cache.cpp
  render/texture_resolve.cpp
  physics/physics_world.cpp
  script/con_interpreter.cpp
  script/gameplay_script.cpp
  net/network_stub.cpp
  net/net.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(bf2engine
  PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${enet_SOURCE_DIR}/include
)

target_link_libraries(bf2engine
  PUBLIC
    ${BF2_SDL_TARGET}
    glm::glm
    bf2glew
  PRIVATE
    bf2miniz
    enet
)

# ENet needs Winsock + multimedia timers on Windows (its own CMake doesn't
# propagate these to consumers).
if(WIN32)
  target_link_libraries(bf2engine PRIVATE ws2_32 winmm)
endif()

target_compile_definitions(bf2engine PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLEW)

if(MSVC)
  target_compile_options(bf2engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(bf2engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
