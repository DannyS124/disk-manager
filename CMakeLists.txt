cmake_minimum_required(VERSION 3.21)
project(disk-manager VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus)

qt_add_executable(disk-manager
    src/main.cpp
    src/udisks.cpp src/udisks.h
    src/format.cpp src/format.h
    src/diskmap.cpp src/diskmap.h
    src/mainwindow.cpp src/mainwindow.h
    src/dialogs.cpp src/dialogs.h
)
target_link_libraries(disk-manager PRIVATE Qt6::Widgets Qt6::DBus)
target_compile_definitions(disk-manager PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_compile_options(disk-manager PRIVATE -Wall -Wextra)

# dev tools, not installed
option(DISK_MANAGER_SELFTEST "Build the loop-device self-test" ON)
if(DISK_MANAGER_SELFTEST)
    qt_add_executable(disk-manager-selftest tests/selftest.cpp src/udisks.cpp src/format.cpp)
    target_link_libraries(disk-manager-selftest PRIVATE Qt6::Core Qt6::DBus)

    qt_add_executable(disk-manager-preview tests/preview.cpp src/dialogs.cpp src/udisks.cpp src/format.cpp)
    target_link_libraries(disk-manager-preview PRIVATE Qt6::Widgets Qt6::DBus)
endif()

include(GNUInstallDirs)
install(TARGETS disk-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(disk-manager.desktop.in disk-manager.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/disk-manager.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
