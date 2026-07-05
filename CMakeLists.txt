cmake_minimum_required(VERSION 3.21)

project(llama_cpp_statusbar_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent)

qt_add_executable(llama_cpp_tray
    src/main.cpp
    resources/resources.qrc
)

file(GLOB TRANSLATION_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/translations/*.json"
)

qt_add_resources(llama_cpp_tray "translations"
    PREFIX "/translations"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/translations"
    FILES ${TRANSLATION_FILES}
)

target_link_libraries(llama_cpp_tray PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

target_compile_options(llama_cpp_tray PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
