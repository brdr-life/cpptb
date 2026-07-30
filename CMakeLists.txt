cmake_minimum_required(VERSION 3.20)

project(cpptb VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})
option(CPPTB_WITH_Z3 "Build and export the optional cpptb::z3 adapter" OFF)

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

add_library(cpptb_vc INTERFACE)
add_library(cpptb::vc ALIAS cpptb_vc)
set_target_properties(cpptb_vc PROPERTIES EXPORT_NAME vc)
target_link_libraries(cpptb_vc INTERFACE cpptb::cpptb)

set(CPPTB_INSTALL_TARGETS cpptb cpptb_vc)
set(CPPTB_CONFIG_WITH_Z3 0)
if(CPPTB_WITH_Z3)
    find_package(PkgConfig REQUIRED)
    # z3::get_full_version(), used by cpptb/z3_random_backend.hpp, first shipped
    # in Z3 4.15.5. Keep in sync with tools/check_environment.py and the
    # CPPTB_MIN_Z3_VERSION in the Makefile.
    pkg_check_modules(CPPTB_Z3 REQUIRED IMPORTED_TARGET "z3 >= 4.15.5")
    add_library(cpptb_z3 INTERFACE)
    add_library(cpptb::z3 ALIAS cpptb_z3)
    set_target_properties(cpptb_z3 PROPERTIES EXPORT_NAME z3)
    target_link_libraries(cpptb_z3 INTERFACE cpptb::cpptb PkgConfig::CPPTB_Z3)
    list(APPEND CPPTB_INSTALL_TARGETS cpptb_z3)
    set(CPPTB_CONFIG_WITH_Z3 1)
endif()

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

    add_executable(cpptb_random_test tests/unit/random_test.cpp)
    target_link_libraries(cpptb_random_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.random COMMAND cpptb_random_test)

    add_executable(cpptb_randomized_test tests/unit/randomized_test.cpp)
    target_link_libraries(cpptb_randomized_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.randomized COMMAND cpptb_randomized_test)

    add_executable(cpptb_coverage_test tests/unit/coverage_test.cpp)
    target_link_libraries(cpptb_coverage_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.coverage COMMAND cpptb_coverage_test)

    add_executable(cpptb_coverage_sv_semantics_test
        tests/unit/coverage_sv_semantics_test.cpp)
    target_link_libraries(cpptb_coverage_sv_semantics_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.coverage_sv_semantics
        COMMAND cpptb_coverage_sv_semantics_test)

    add_executable(cpptb_test_api_test tests/unit/test_api_test.cpp)
    target_link_libraries(cpptb_test_api_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.test_api COMMAND cpptb_test_api_test)

    add_executable(cpptb_hierarchy_test tests/unit/hierarchy_test.cpp)
    target_compile_definitions(cpptb_hierarchy_test
                               PRIVATE CPPTB_HIERARCHY_DISCOVERY)
    target_link_libraries(cpptb_hierarchy_test PRIVATE cpptb::cpptb)
    add_test(NAME cpptb.hierarchy COMMAND cpptb_hierarchy_test)

    add_executable(cpptb_vc_components_test tests/unit/components_test.cpp)
    target_link_libraries(cpptb_vc_components_test PRIVATE cpptb::vc)
    add_test(NAME cpptb_vc.components COMMAND cpptb_vc_components_test)

    add_executable(cpptb_vc_transaction_recording_test
                   tests/unit/transaction_recording_test.cpp)
    target_link_libraries(cpptb_vc_transaction_recording_test
                          PRIVATE cpptb::vc)
    add_test(NAME cpptb_vc.transaction_recording
             COMMAND cpptb_vc_transaction_recording_test)

    if(CPPTB_WITH_Z3)
        add_executable(cpptb_z3_random_test
                       tests/unit/z3_random_backend_test.cpp)
        target_link_libraries(cpptb_z3_random_test PRIVATE cpptb::z3)
        add_test(NAME cpptb.z3_random COMMAND cpptb_z3_random_test)
    endif()

    function(cpptb_add_public_header_checks directory link_target)
        file(GLOB public_headers
             RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
             "${CMAKE_CURRENT_SOURCE_DIR}/include/${directory}/*.hpp")
        foreach(header IN LISTS public_headers)
            if(header STREQUAL "cpptb/z3_random_backend.hpp" AND
               NOT CPPTB_WITH_Z3)
                continue()
            endif()
            set(header_link_target "${link_target}")
            if(header STREQUAL "cpptb/z3_random_backend.hpp")
                set(header_link_target cpptb::z3)
            endif()
            string(MAKE_C_IDENTIFIER "${header}" target_suffix)
            set(source
                "${CMAKE_CURRENT_BINARY_DIR}/header_checks/${target_suffix}.cpp")
            file(WRITE "${source}" "#include <${header}>\n")
            add_library("cpptb_header_${target_suffix}" OBJECT "${source}")
            target_link_libraries("cpptb_header_${target_suffix}"
                                  PRIVATE "${header_link_target}")
        endforeach()
    endfunction()

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header_checks")
    cpptb_add_public_header_checks(cpptb cpptb::cpptb)
    cpptb_add_public_header_checks(cpptb_vc cpptb::vc)

endif()

install(TARGETS ${CPPTB_INSTALL_TARGETS} EXPORT cpptbTargets)
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
