add_executable(project_dalian main.cpp)
target_include_directories(project_dalian PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(project_dalian PRIVATE bf2engine)
