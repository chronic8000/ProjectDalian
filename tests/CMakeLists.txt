add_executable(bf2_tests test_formats.cpp)
target_link_libraries(bf2_tests PRIVATE bf2engine)
add_test(NAME formats COMMAND bf2_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(bf2_test_minimal test_minimal.cpp)
target_link_libraries(bf2_test_minimal PRIVATE bf2engine)
