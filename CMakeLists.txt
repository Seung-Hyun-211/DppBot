cmake_minimum_required(VERSION 3.20)

project(dppTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# UTF-8 인코딩 설정 (MSVC용)
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows")
endif()

if(UNIX AND EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux")
endif()

add_compile_definitions(JSON_USE_IMPLICIT_CONVERSIONS=0)

find_package(dpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(dppTest
    src/main.cpp
    src/DiscordBotClient.cpp
    src/FileLoader.cpp
)

target_link_libraries(dppTest PRIVATE
    dpp::dpp
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
