cmake_minimum_required(VERSION 3.12)
project(chat_app_server C)

set(CMAKE_C_STANDARD 99)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(chat_app_server main.c)

target_link_libraries(chat_app_server Threads::Threads)