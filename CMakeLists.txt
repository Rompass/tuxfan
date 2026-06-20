cmake_minimum_required(VERSION 3.10)
project(TuxFanControl C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L")

find_package(Curses REQUIRED)
find_package(Threads REQUIRED)

add_executable(tuxfan tuxfan.c)

target_link_libraries(tuxfan PRIVATE ${CURSES_LIBRARIES} Threads::Threads)
target_include_directories(tuxfan PRIVATE ${CURSES_INCLUDE_DIR})

install(TARGETS tuxfan DESTINATION /usr/local/bin)
