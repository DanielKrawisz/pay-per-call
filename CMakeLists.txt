cmake_minimum_required (VERSION 3.16)
project (PayPerCall CXX)

find_package (cosmos_lib CONFIG REQUIRED)

add_executable (PayPerCall pay-per-call.cpp)

target_link_libraries (PayPerCall PUBLIC cosmos_lib::cosmos_lib gigamonkey::gigamonkey data::data)
target_include_directories (PayPerCall PUBLIC include)

target_compile_features (PayPerCall PUBLIC cxx_std_20)
set_target_properties (PayPerCall PROPERTIES CXX_EXTENSIONS OFF)

add_definitions ("-DHAS_BOOST")

# option (PACKAGE_TESTS "Build the tests" ON)

#if (PACKAGE_TESTS)
#    include (CTest)
#    find_package (GTest REQUIRED)
#    enable_testing ()
#    add_subdirectory (test)
#endif ()

install (TARGETS PayPerCall)

