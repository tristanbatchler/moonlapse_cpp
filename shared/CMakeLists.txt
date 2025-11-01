add_library(moonlapse_shared INTERFACE)

target_link_libraries(moonlapse_shared INTERFACE c++)

set_property(TARGET moonlapse_shared PROPERTY
  INTERFACE_COMPILE_FEATURES cxx_std_23)

target_include_directories(moonlapse_shared INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  /usr/lib/llvm-20/include/c++/v1
)