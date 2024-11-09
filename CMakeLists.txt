cmake_minimum_required(VERSION 3.10)
project(yt2mp3)

set(CMAKE_CXX_STANDARD 11)

find_package(cpprestsdk REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)

add_executable(yt2mp3-API yt2mp3-API.cpp yt2mp3.cpp)

include_directories(${Boost_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

target_link_libraries(yt2mp3-API PRIVATE cpprestsdk::cpprest ${Boost_LIBRARIES} ${OPENSSL_LIBRARIES})


