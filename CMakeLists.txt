cmake_minimum_required(VERSION 3.22)

project(BinaryXorProcessor
    VERSION 1.0.0
    DESCRIPTION "Background XOR file processor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)

add_library(binaryxor_core STATIC
    src/fileprocessor.cpp
    src/fileprocessor.h
    src/processingsettings.h
)

target_include_directories(binaryxor_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(binaryxor_core PUBLIC
    Qt${QT_VERSION_MAJOR}::Core
)

add_executable(BinaryXorProcessor WIN32
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
)

target_link_libraries(BinaryXorProcessor PRIVATE
    binaryxor_core
    Qt${QT_VERSION_MAJOR}::Widgets
)

if(MSVC)
    target_compile_options(binaryxor_core PRIVATE /W4 /permissive-)
    target_compile_options(BinaryXorProcessor PRIVATE /W4 /permissive-)
else()
    target_compile_options(binaryxor_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(BinaryXorProcessor PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(BinaryXorProcessor PROPERTIES
    OUTPUT_NAME "BinaryXorProcessor"
    WIN32_EXECUTABLE TRUE
)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS Test)
    if(TARGET Qt${QT_VERSION_MAJOR}::Test)
        add_executable(BinaryXorProcessorTests
            tests/tst_fileprocessor.cpp
        )
        target_link_libraries(BinaryXorProcessorTests PRIVATE
            binaryxor_core
            Qt${QT_VERSION_MAJOR}::Test
        )
        add_test(NAME BinaryXorProcessorTests COMMAND BinaryXorProcessorTests)
        set_tests_properties(BinaryXorProcessorTests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>"
        )

        add_executable(BinaryXorProcessorUiTests
            tests/tst_mainwindow.cpp
            src/mainwindow.cpp
            src/mainwindow.h
        )
        target_link_libraries(BinaryXorProcessorUiTests PRIVATE
            binaryxor_core
            Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::Test
        )
        add_test(NAME BinaryXorProcessorUiTests COMMAND BinaryXorProcessorUiTests)
        set_tests_properties(BinaryXorProcessorUiTests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>"
        )
    endif()
endif()
