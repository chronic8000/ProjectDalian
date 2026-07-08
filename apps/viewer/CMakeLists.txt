add_executable(bf2viewer WIN32 main.cpp)
target_link_libraries(bf2viewer PRIVATE bf2engine SDL2::SDL2main)
