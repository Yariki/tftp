cmake_minimum_required(VERSION 3.16)
project(tftp_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


add_executable(ftf_server main.cpp)


if(WIN32)
    target_link_libraries(tftp_server PRIVATE  ws2_32)
endif()

if(MSVC)
    target_compile_options(tftp_server PRIVATE /W4)
else()
    target_compile_options(tftp_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
