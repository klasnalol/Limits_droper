cmake_minimum_required(VERSION 3.16)
project(Limits_droper LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)

add_executable(mchbar_read mchbar_read.c)

add_executable(mchbar_pl_write mchbar_pl_write.c)

add_executable(limits_ui limits_ui.c)
target_link_libraries(limits_ui m)

find_package(Qt6 COMPONENTS Widgets QUIET)
find_package(Qt5 COMPONENTS Widgets QUIET)

set(BUILD_QT ON)

if (Qt6_FOUND)
    set(QT_LIBS Qt6::Widgets)
elseif (Qt5_FOUND)
    set(QT_LIBS Qt5::Widgets)
else()
    set(BUILD_QT OFF)
    message(FATAL_ERROR
        "Qt Widgets development package not found. Install Qt6 or Qt5 dev files "
        "(Qt6Config.cmake / Qt5Config.cmake), or set CMAKE_PREFIX_PATH / Qt*_DIR.")
endif()

if (BUILD_QT)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    add_executable(limits_ui_qt
       qt_ui/main.cpp
    )
    target_link_libraries(limits_ui_qt PRIVATE ${QT_LIBS})
endif()

