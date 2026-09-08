cmake_minimum_required(VERSION 3.16)

# Reuse the board support package from this repository.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(idf_camera_webserver)
