cmake_minimum_required(VERSION 3.14)
project(CustomRelationalDB VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Crow and Asio Dependencies
include(FetchContent)

add_definitions(-DCROW_STANDALONE)
add_definitions(-DASIO_STANDALONE)

FetchContent_Declare(
  asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-24-0
)

FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
  GIT_TAG v1.1.0
)

FetchContent_MakeAvailable(asio)
FetchContent_GetProperties(crow)
if(NOT crow_POPULATED)
  FetchContent_Populate(crow)
endif()

include_directories(include)
include_directories(${asio_SOURCE_DIR}/asio/include)
include_directories(${crow_SOURCE_DIR}/include)

# Find Threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Main Executable
add_executable(relational_db 
    src/main.cpp 
    src/disk_manager.cpp 
    src/slotted_page.cpp 
    src/clock_replacer.cpp 
    src/buffer_pool_manager.cpp 
    src/b_plus_tree.cpp 
    src/executor.cpp
    src/wal.cpp
)

target_link_libraries(relational_db PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(relational_db PRIVATE ws2_32 mswsock)
endif()
