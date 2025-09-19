cmake_minimum_required(VERSION 3.16)
project(qmp_bridge)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(Threads REQUIRED)
find_package(Boost COMPONENTS system REQUIRED)
find_package(PkgConfig REQUIRED)

# Find websocketpp (header-only)
find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/config/asio_no_tls.hpp
    PATHS /usr/include /usr/local/include /opt/homebrew/include)

if(NOT WEBSOCKETPP_INCLUDE_DIR)
    message(FATAL_ERROR "websocketpp not found. Install with: brew install websocketpp")
endif()

# Find jsoncpp
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Main executable
add_executable(qmp_bridge qmp_bridge.cpp)

target_include_directories(qmp_bridge PRIVATE
    ${WEBSOCKETPP_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
)

target_link_libraries(qmp_bridge PRIVATE
    Threads::Threads
    ${Boost_LIBRARIES}
    ${JSONCPP_LIBRARIES}
)

# On macOS, you may need additional flags
if(APPLE)
    target_compile_definitions(qmp_bridge PRIVATE _WEBSOCKETPP_CPP11_STL_)
endif()