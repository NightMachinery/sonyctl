cmake_minimum_required(VERSION 3.31 FATAL_ERROR)
project(sonyctl CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT APPLE)
    message(FATAL_ERROR "sonyctl currently only supports macOS (IOBluetooth transport)")
endif ()

include(FetchContent)

# libmdr / libmdr-bt from SonyHeadphonesClient. Pinned: v1-compat is a moving
# branch; bump the SHA deliberately after testing.
set(MDR_BUILD_CLIENT OFF)
set(MDR_ENABLE_CODEGEN OFF)
set(BUILD_TESTING OFF)
FetchContent_Declare(
    SonyHeadphonesClient
    GIT_REPOSITORY https://github.com/mos9527/SonyHeadphonesClient
    GIT_TAG ae71d32e83c9ebc34993936130074257a906bdfa
)
FetchContent_MakeAvailable(SonyHeadphonesClient)

# Upstream's libmdr-bt declares its include dir as ${CMAKE_SOURCE_DIR}/libmdr-bt/include,
# which resolves to *this* project when consumed via FetchContent. Point the
# targets at the real location.
foreach (_mdr_bt_target mdr-bt mdr-bt_shared)
    target_include_directories(${_mdr_bt_target} PUBLIC
        "${sonyheadphonesclient_SOURCE_DIR}/libmdr-bt/include")
endforeach ()

add_executable(sonyctl src/main.cpp)
target_link_libraries(sonyctl PRIVATE mdr mdr-bt)

install(TARGETS sonyctl RUNTIME DESTINATION bin)
