cmake_minimum_required(VERSION 3.10)
project(Project1)

set(CMAKE_CXX_STANDARD 20)

# Tell CMake where to find the header files
include_directories(include)

# Tell CMake to compile main.cpp AND all the new .cpp files
add_executable(Project1
		src/main.cpp
    src/Common.cpp
    src/Parsers.cpp
    src/Server.cpp
)

# Tell CMake to link the POSIX Threads library (required for macOS/Linux)
find_package(Threads REQUIRED)
target_link_libraries(Project1 Threads::Threads)