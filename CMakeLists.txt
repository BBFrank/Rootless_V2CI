cmake_minimum_required(VERSION 3.10)
project(v2ci C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g")

include_directories(src/include)

set(START_SOURCES
    src/main.c
    src/project_worker.c
    src/build_thread.c
    src/lib/init/load_config.c
    src/lib/utils/utils.c
    src/lib/utils/scripts_runner.c
)

set(STOP_SOURCES
    src/stop.c
    src/lib/init/load_config.c
    src/lib/utils/utils.c
)

add_executable(v2ci_start ${START_SOURCES})
add_executable(v2ci_stop ${STOP_SOURCES})

find_package(Threads REQUIRED)
set(LIBYAML "/usr/lib/x86_64-linux-gnu/libyaml.a")
target_link_libraries(v2ci_start PRIVATE Threads::Threads execs ${LIBYAML})
target_link_libraries(v2ci_stop PRIVATE ${LIBYAML})

target_link_options(v2ci_start PRIVATE "-static")
target_link_options(v2ci_stop PRIVATE "-static")
