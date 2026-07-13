cmake_minimum_required(VERSION 3.20)

project(cpptb_package_consumer LANGUAGES CXX)

find_package(cpptb CONFIG REQUIRED)

add_executable(cpptb_package_consumer main.cpp)
target_link_libraries(cpptb_package_consumer PRIVATE cpptb::cpptb)
