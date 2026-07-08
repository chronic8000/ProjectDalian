cmake_minimum_required(VERSION 3.24)
project(bf2respawn VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BF2RESPAWN_BUILD_TESTS "Build unit tests" ON)
option(BF2RESPAWN_BUILD_VIEWER "Build mesh viewer app" ON)
option(BF2RESPAWN_BUILD_DALIAN "Build the Project Dalian war-sim app" ON)
option(BF2RESPAWN_BUILD_SANDBOX "Build gameplay sandbox app" ON)

add_subdirectory(engine)
add_subdirectory(tools)

if(BF2RESPAWN_BUILD_VIEWER)
    add_subdirectory(apps/viewer)
endif()

if(BF2RESPAWN_BUILD_DALIAN)
    add_subdirectory(apps/dalian)
endif()

if(BF2RESPAWN_BUILD_SANDBOX)
    add_subdirectory(apps/sandbox)
endif()

if(BF2RESPAWN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
