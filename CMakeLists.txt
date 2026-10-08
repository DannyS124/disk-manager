cmake_minimum_required(VERSION 3.21)
project(diskforge
    VERSION 0.4.0
    DESCRIPTION "Windows-style disk manager for Linux, built on UDisks2"
    HOMEPAGE_URL "https://github.com/DannyS124/diskforge"
    LANGUAGES CXX)

set(APP_ID "io.github.DannyS124.DiskForge")
set(APP_AUTHOR "Danny S")
set(APP_AUTHOR_URL "https://github.com/DannyS124")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus Network)

qt_add_executable(diskforge
    src/main.cpp
    src/udisks.cpp src/udisks.h
    src/format.cpp src/format.h
    src/diskmap.cpp src/diskmap.h
    src/mainwindow.cpp src/mainwindow.h
    src/dialogs.cpp src/dialogs.h
    src/about.cpp src/about.h
    src/updates.cpp src/updates.h
)
qt_add_resources(diskforge "app_data" PREFIX "/" FILES
    data/help.html
    data/${APP_ID}.svg
    LICENSE)
target_link_libraries(diskforge PRIVATE Qt6::Widgets Qt6::DBus Qt6::Network)
set(APP_DEFINITIONS
    APP_VERSION="${PROJECT_VERSION}"
    APP_ID="${APP_ID}"
    APP_AUTHOR="${APP_AUTHOR}"
    APP_AUTHOR_URL="${APP_AUTHOR_URL}"
    APP_HOMEPAGE="${PROJECT_HOMEPAGE_URL}")
target_compile_definitions(diskforge PRIVATE ${APP_DEFINITIONS})
target_compile_options(diskforge PRIVATE -Wall -Wextra)

# dev tools, not installed
option(DISKFORGE_SELFTEST "Build the loop-device self-test" ON)
if(DISKFORGE_SELFTEST)
    qt_add_executable(diskforge-selftest tests/selftest.cpp src/udisks.cpp src/format.cpp)
    target_link_libraries(diskforge-selftest PRIVATE Qt6::Core Qt6::DBus)

    qt_add_executable(diskforge-preview tests/preview.cpp src/dialogs.cpp src/about.cpp src/udisks.cpp src/format.cpp)
    qt_add_resources(diskforge-preview "preview_data" PREFIX "/" FILES data/help.html data/${APP_ID}.svg LICENSE)
    target_link_libraries(diskforge-preview PRIVATE Qt6::Widgets Qt6::DBus)
    target_compile_definitions(diskforge-preview PRIVATE ${APP_DEFINITIONS})
endif()

include(GNUInstallDirs)
configure_file(data/${APP_ID}.desktop.in ${APP_ID}.desktop @ONLY)
configure_file(data/${APP_ID}.metainfo.xml.in ${APP_ID}.metainfo.xml @ONLY)
configure_file(data/diskforge.1.in diskforge.1 @ONLY)

install(TARGETS diskforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/diskforge.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
