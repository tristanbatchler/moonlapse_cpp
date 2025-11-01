cmake_minimum_required(VERSION 3.26)

project(moonlapse_multiplayer
  VERSION 0.1.0
  DESCRIPTION "Simple terminal multiplayer game scaffold"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if(UNIX AND NOT APPLE)
    add_compile_options(-stdlib=libc++)
    link_libraries(c++ c++abi)
  endif()
endif()

add_compile_definitions(_LIBCPP_ENABLE_CXX23_PRINT)

add_subdirectory(shared)
add_subdirectory(server)
add_subdirectory(client)
