# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# UŻYWAMY PEŁNEJ ŚCIEŻKI SYSTEMOWEJ DO FOLDERU COMPONENTS
# set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")



# PODAJEMY DOSŁOWNĄ ŚCIEŻKĘ DO TWOICH STEROWNIKÓW NA DYSKU D
set(EXTRA_COMPONENT_DIRS "D:/MY_ESP32_TRY/P4/JC8012P4A1-IDF-DSI/JC8012P4A1-IDF-DSI/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(JC8012P4A1-IDF-DSI)
