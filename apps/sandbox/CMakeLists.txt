add_executable(bf2sandbox main.cpp)
target_link_libraries(bf2sandbox PRIVATE bf2engine)

add_executable(track_audit track_audit_main.cpp)
target_link_libraries(track_audit PRIVATE bf2engine)

add_executable(road_audit road_audit_main.cpp)
target_link_libraries(road_audit PRIVATE bf2engine)


