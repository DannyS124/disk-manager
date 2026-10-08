# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(diskforge
    VERSION 0.6.0
    DESCRIPTION "Disk manager for Linux in the style of Windows Disk Management"
    HOMEPAGE_URL "https://github.com/DannyS124/diskforge"
    LANGUAGES CXX)

set(APP_ID "io.github.DannyS124.DiskForge")
set(APP_AUTHOR "Danny S")
set(APP_AUTHOR_URL "https://github.com/DannyS124")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets DBus Network LinguistTools)
qt_standard_project_setup(I18N_SOURCE_LANGUAGE en)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

set(APP_DEFINITIONS
    APP_VERSION="${PROJECT_VERSION}"
    APP_ID="${APP_ID}"
    APP_AUTHOR="${APP_AUTHOR}"
    APP_AUTHOR_URL="${APP_AUTHOR_URL}"
    APP_HOMEPAGE="${PROJECT_HOMEPAGE_URL}")

# Everything that talks to UDisks2 or touches devices, shared with the test tools.
add_library(diskforge-core STATIC
    src/udisks.cpp src/udisks.h
    src/operations.cpp src/dbusnames.h
    src/format.cpp src/format.h
    src/blockio.cpp src/blockio.h
    src/systemd.cpp src/systemd.h
    src/imagewriter.cpp src/imagewriter.h
    src/benchmark.cpp src/benchmark.h
    src/addons.cpp src/addons.h
    src/surfacescan.cpp src/surfacescan.h
    src/gpt.cpp src/gpt.h
    src/blockcopy.cpp src/blockcopy.h
    src/blockmapdata.cpp src/blockmapdata.h
    src/fswalk.cpp src/fswalk.h src/squarify.h
    src/clone.cpp src/clone.h
    src/imagebackup.cpp src/imagebackup.h
    src/rescuecopy.cpp src/rescuecopy.h
)
target_link_libraries(diskforge-core PUBLIC Qt6::Core Qt6::DBus PRIVATE PkgConfig::ZSTD)
target_include_directories(diskforge-core PUBLIC src)
target_compile_definitions(diskforge-core PUBLIC ${APP_DEFINITIONS})
target_compile_options(diskforge-core PRIVATE -Wall -Wextra)

# Windows and dialogs, shared by the app, the dialog preview and the window test.
add_library(diskforge-ui STATIC
    src/diskmap.cpp src/diskmap.h
    src/mainwindow.cpp src/mainwindow.h
    src/dialogs.cpp src/dialogs.h
    src/tools.cpp src/tools.h
    src/blockmapwidget.cpp src/blockmapwidget.h
    src/jobui.cpp src/jobui.h
    src/addonsdialog.cpp src/addonsdialog.h
    src/about.cpp src/about.h
    src/updates.cpp src/updates.h
    src/translations.cpp src/translations.h
)
target_link_libraries(diskforge-ui PUBLIC diskforge-core Qt6::Widgets Qt6::Network)
target_compile_options(diskforge-ui PRIVATE -Wall -Wextra)

# Help page, icon and license text, built into each program that shows them.
function(diskforge_add_data target)
    qt_add_resources(${target} "${target}_data" PREFIX "/" FILES data/help.html data/${APP_ID}.svg LICENSE)
endfunction()

qt_add_executable(diskforge src/main.cpp)
diskforge_add_data(diskforge)
target_link_libraries(diskforge PRIVATE diskforge-ui)
target_compile_options(diskforge PRIVATE -Wall -Wextra)


# dev tools, not installed
option(DISKFORGE_SELFTEST "Build the loop-device self-test" ON)
if(DISKFORGE_SELFTEST)
    qt_add_executable(diskforge-selftest tests/selftest.cpp tests/testkit.cpp tests/testkit.h
        tests/test_gpt.cpp tests/test_copy.cpp tests/test_blockmap.cpp
        tests/test_usage.cpp tests/test_backup.cpp
        tests/test_rescue.cpp tests/test_clone.cpp)
    target_link_libraries(diskforge-selftest PRIVATE diskforge-core)
    target_compile_definitions(diskforge-selftest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-preview tests/preview.cpp)
    diskforge_add_data(diskforge-preview)
    target_link_libraries(diskforge-preview PRIVATE diskforge-ui)

    qt_add_executable(diskforge-uitest tests/uitest.cpp)
    diskforge_add_data(diskforge-uitest)
    target_link_libraries(diskforge-uitest PRIVATE diskforge-ui)
endif()

# English plural forms ("1 sector", "2 sectors"); other languages get their own
# i18n/diskforge_<lang>.ts (see CONTRIBUTING.md).
set(TRANSLATED_TARGETS diskforge)
if(DISKFORGE_SELFTEST)
    list(APPEND TRANSLATED_TARGETS diskforge-preview)
endif()
qt_add_translations(TARGETS ${TRANSLATED_TARGETS}
    SOURCE_TARGETS diskforge-core diskforge-ui diskforge
    TS_FILE_DIR i18n
    PLURALS_TS_FILE i18n/diskforge_en.ts
    RESOURCE_PREFIX /i18n)

include(GNUInstallDirs)
configure_file(data/${APP_ID}.desktop.in ${APP_ID}.desktop @ONLY)
configure_file(data/${APP_ID}.metainfo.xml.in ${APP_ID}.metainfo.xml @ONLY)
configure_file(data/diskforge.1.in diskforge.1 @ONLY)

install(TARGETS diskforge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES data/${APP_ID}.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/diskforge.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
