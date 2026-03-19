cmake_minimum_required(VERSION 3.10.0)
project(libevent_server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED
    libevent_core
    libevent_extra
    libevent_pthreads)

add_executable(server server.cpp)
add_executable(client client.cpp)

target_include_directories(server PRIVATE ${LIBEVENT_INCLUDE_DIRS})
target_link_libraries(server PRIVATE ${LIBEVENT_LIBRARIES})
target_link_libraries(client PRIVATE Threads::Threads)

