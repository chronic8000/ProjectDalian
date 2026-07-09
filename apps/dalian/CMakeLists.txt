include(FetchContent)

# SDL_mixer needs the same SDL2 that bf2engine already fetched/built.
if(EXISTS "${CMAKE_BINARY_DIR}/_deps/sdl2-build")
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/_deps/sdl2-build")
endif()

FetchContent_Declare(
  SDL_mixer
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
  GIT_TAG release-2.8.1
  GIT_SHALLOW TRUE
)

set(SDL2MIXER_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_CMD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3 ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL_mixer)

add_executable(project_dalian main.cpp menu.cpp menu_background.cpp multiplayer_menu.cpp server_discovery.cpp menu_music.cpp audio_context.cpp game_audio.cpp game_sim.cpp conquest_sim.cpp game_snapshot.cpp map_conquest_parser.cpp game_logic_parser.cpp tweak_parser.cpp minimap_projector.cpp radial_menu.cpp hitbox_zones.cpp bf2_effects.cpp weapon_profile.cpp soldier_anim.cpp soldier_anim_library.cpp vehicle_tweak_profile.cpp vehicle_wing_profile.cpp vehicle_air_profile.cpp vehicle_weapon_profile.cpp projectile_profile.cpp ambient_emitter.cpp)
target_include_directories(project_dalian PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(project_dalian PRIVATE bf2engine SDL2_mixer::SDL2_mixer)

add_executable(dalian_util_tests dalian_util_tests.cpp conquest_sim.cpp game_snapshot.cpp map_conquest_parser.cpp game_logic_parser.cpp tweak_parser.cpp minimap_projector.cpp radial_menu.cpp hitbox_zones.cpp vehicle_air_profile.cpp vehicle_weapon_profile.cpp projectile_profile.cpp ambient_emitter.cpp bf2_effects.cpp)
target_link_libraries(dalian_util_tests PRIVATE bf2engine)
add_test(NAME dalian_utils COMMAND dalian_util_tests)
if(WIN32)
  target_link_libraries(project_dalian PRIVATE ws2_32)
  add_custom_command(TARGET project_dalian POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:SDL2::SDL2>
      $<TARGET_FILE_DIR:project_dalian>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:SDL2_mixer::SDL2_mixer>
      $<TARGET_FILE_DIR:project_dalian>
    COMMENT "Copying SDL2 / SDL2_mixer runtime DLLs")
endif()

add_custom_command(TARGET project_dalian POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/music
    $<TARGET_FILE_DIR:project_dalian>/music
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/menu
    $<TARGET_FILE_DIR:project_dalian>/menu
  COMMENT "Copying menu music and background assets")
