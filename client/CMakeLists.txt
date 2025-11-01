add_executable(moonlapse_client main.cpp)

set_target_properties(moonlapse_client PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_include_directories(moonlapse_client PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../shared
  /usr/lib/llvm-20/include/c++/v1
)

if (WIN32)
  target_link_libraries(moonlapse_client PRIVATE
    moonlapse_shared
    Threads::Threads
    pdcurses
  )
else()
  find_package(Curses REQUIRED)
  target_include_directories(moonlapse_client PRIVATE ${CURSES_INCLUDE_DIR})
  target_link_libraries(moonlapse_client PRIVATE
    moonlapse_shared
    Threads::Threads
    ${CURSES_LIBRARIES}
  )
endif()
