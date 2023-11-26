project(CS425-MP4)

set(CMAKE_C_COMPILER "/usr/bin/clang")
set (CMAKE_C_FLAGS "-std=c++20 -O3 -Wall -Wextra -Wno-sign-compare -pthread")
set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
set (CMAKE_CXX_FLAGS "-std=c++20 -O3 -Wall -Wextra -Wno-sign-compare -pthread")
set(CMAKE_CXX_STANDARD 20)

find_package(TensorflowCC REQUIRED)
add_executable(idunno_server src/idunno/idunno_server.cc src/tensorflow/label_image.cc src/tensorflow/multibox_detector.cc)
add_executable(grep_server src/distributed_grep/grep_server.cc)
add_executable(grep_client src/distributed_grep/grep_client.cc)
target_link_libraries(idunno_server TensorflowCC::TensorflowCC)
