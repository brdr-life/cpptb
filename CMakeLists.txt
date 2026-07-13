cmake_minimum_required(VERSION 3.20)

project(cpptb VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})

add_library(cpptb INTERFACE)
add_library(cpptb::cpptb ALIAS cpptb)
target_compile_features(cpptb INTERFACE cxx_std_20)
target_include_directories(
    cpptb
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${VERILATOR_ROOT}/include>
        $<BUILD_INTERFACE:${VERILATOR_ROOT}/include/vltstd>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(BUILD_TESTING)
    add_executable(cpptb_coro_runtime_test tests/unit/coro_runtime_test.cpp)
    target_link_libraries(cpptb_coro_runtime_test PRIVATE cpptb::cpptb)
    target_compile_definitions(
        cpptb_coro_runtime_test
        PRIVATE
            CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
            CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
    )
    add_test(NAME cpptb.coro_runtime COMMAND cpptb_coro_runtime_test)

    add_executable(cpptb_packed_value_test tests/unit/packed_value_test.cpp)
    target_link_libraries(cpptb_packed_value_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.packed_value COMMAND cpptb_packed_value_test)
endif()

install(TARGETS cpptb EXPORT cpptbTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
    EXPORT cpptbTargets
    FILE cpptbTargets.cmake
    NAMESPACE cpptb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cpptb
)

configure_package_config_file(
    cmake/cpptbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cpptbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cpptb
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cpptbConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cpptbConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cpptbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cpptb
)
