set(SOURCES execution_manager.cpp execution_common.cpp)
add_library(execution STATIC ${SOURCES})

target_link_libraries(execution system record transaction planner)