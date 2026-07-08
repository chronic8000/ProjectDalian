add_executable(bf2snapshot main.cpp)
target_include_directories(bf2snapshot PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(bf2snapshot PRIVATE bf2engine glm::glm)
