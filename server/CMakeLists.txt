add_executable(moonlapse_server main.cpp)

set_target_properties(moonlapse_server PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(moonlapse_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../shared
  /usr/lib/llvm-20/include/c++/v1
)

target_link_libraries(moonlapse_server PRIVATE
  moonlapse_shared
  Threads::Threads
)