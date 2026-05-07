cmake_minimum_required(VERSION 3.20)
project(mdit_install_smoke C)

# Locate the staged mdit package.
find_package(mdit REQUIRED)

add_executable(smoke smoke.c)
target_link_libraries(smoke PRIVATE mdit::mdit)
