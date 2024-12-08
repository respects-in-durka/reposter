cmake_minimum_required(VERSION 3.29)
project(njinks)

set(CMAKE_CXX_STANDARD 20)
set(Td_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/td/lib/cmake/Td")
set(Crow_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/crow/lib/cmake/Crow")
set(amqpcpp_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/amqp/cmake")
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/boost/include")

find_package(Td 1.8.40 REQUIRED)
find_package(Crow REQUIRED)
find_package(amqpcpp REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

add_executable(njinks main.cpp)
target_include_directories(njinks PRIVATE ${Boost_INCLUDE_DIR})
target_link_libraries(njinks PRIVATE Td::TdStatic amqpcpp Boost::system)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(njinks PRIVATE DEBUG=1)
    target_link_libraries(njinks PRIVATE Crow::Crow)
endif()

add_executable(njinks_authorizer authorizer/authorizer.cpp)
target_link_libraries(njinks_authorizer PRIVATE Td::TdStatic)
target_link_libraries(njinks_authorizer PUBLIC Crow::Crow)
