cmake_minimum_required(VERSION 3.10)
project(flow-tester)

set(CMAKE_CXX_STANDARD 17)

include(FindPkgConfig)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
else()
  pkg_search_module(SPDLOG REQUIRED spdlog)
endif()
pkg_search_module(LIBUV REQUIRED libuv)

find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(flow_sender sender.cpp protocol.cpp protocol.h)

add_executable(flow_receiver receiver.cpp protocol.cpp protocol.h ReorderBuffer.cpp ReorderBuffer.h)

add_executable(reorder_test ReorderBuffer.cpp ReorderBuffer.h reorder_test.cpp)

set(all_apps flow_sender flow_receiver reorder_test)

foreach (app ${all_apps})
    target_include_directories(${app} PRIVATE
            ${Boost_INCLUDE_DIRS}
            ${SPDLOG_INCLUDE_DIRS}
            ${LIBUV_INCLUDE_DIRS}
	    /usr/local/include
            )

    target_link_directories(${app} PRIVATE
            ${SPDLOG_STATIC_LIBRARY_DIRS}
            ${LIBUV_STATIC_LIBRARY_DIRS}
            )

    target_link_libraries(${app} PRIVATE
            ${Boost_LIBRARIES}
            ${SPDLOG_STATIC_LIBRARIES}
            ${LIBUV_STATIC_LIBRARIES}
            pthread sqlite3
            )
endforeach ()

add_custom_target(everything)

add_dependencies(everything ${all_apps})

