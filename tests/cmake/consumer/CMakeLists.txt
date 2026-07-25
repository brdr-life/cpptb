cmake_minimum_required(VERSION 3.20)

project(cpptb_package_consumer LANGUAGES CXX)

find_package(cpptb CONFIG REQUIRED)

add_executable(cpptb_package_consumer main.cpp)
target_link_libraries(cpptb_package_consumer PRIVATE cpptb::vc)

# cpptb::z3 only exists when the package was installed with CPPTB_WITH_Z3.
# Adapting here lets the install path be verified on hosts whose Z3 predates
# the 4.15.5 the adapter needs, instead of skipping the check entirely.
if(TARGET cpptb::z3)
    target_link_libraries(cpptb_package_consumer PRIVATE cpptb::z3)
    target_compile_definitions(cpptb_package_consumer PRIVATE CPPTB_CONSUMER_WITH_Z3)
endif()
