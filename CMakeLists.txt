# SPDX-FileCopyrightText: 2026 Danny S
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.21)
project(diskforge
    VERSION 0.5.0
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
# Ed25519, for the signature on the online add-on list
find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)

# Memory and undefined-behaviour checks for the crash tests (release.sh builds this in build-asan).
option(DISKFORGE_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(DISKFORGE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

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
    src/cleanup.cpp src/cleanup.h
    src/snapper.cpp src/snapper.h
    src/signature.cpp src/signature.h
    src/outputfilter.cpp src/outputfilter.h
    src/health.cpp src/health.h
    src/firmware.cpp src/firmware.h
    src/btrfscheck.cpp src/btrfscheck.h
)
target_link_libraries(diskforge-core PUBLIC Qt6::Core Qt6::DBus PRIVATE PkgConfig::ZSTD OpenSSL::Crypto)
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
    src/copydialogs.cpp src/copydialogs.h
    src/usagedialog.cpp src/usagedialog.h
    src/systemtools.cpp src/systemtools.h
    src/erasedialog.cpp src/erasedialog.h
    src/addonsdialog.cpp src/addonsdialog.h
    src/addonprompt.cpp src/addonprompt.h
    src/addonform.cpp src/addonform.h
    src/addonmaker.cpp src/addonmaker.h
    src/addonoutput.cpp src/addonoutput.h
    src/noticebar.cpp src/noticebar.h
    src/typedialog.cpp src/typedialog.h
    src/catalogdialog.cpp src/catalogdialog.h
    src/about.cpp src/about.h
    src/updates.cpp src/updates.h
    src/translations.cpp src/translations.h
    src/theme.cpp src/theme.h
    src/thememaker.cpp src/thememaker.h
)
target_link_libraries(diskforge-ui PUBLIC diskforge-core Qt6::Widgets Qt6::Network)
target_compile_options(diskforge-ui PRIVATE -Wall -Wextra)

# Help page, icon and license text, built into each program that shows them.
function(diskforge_add_data target)
    qt_add_resources(${target} "${target}_data" PREFIX "/" FILES data/help.html data/${APP_ID}.svg LICENSE)
    qt_add_resources(${target} "${target}_themes" PREFIX "/themes" BASE data/themes
        FILES data/themes/classic.json data/themes/deadshadow.json data/themes/high-contrast.json)
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
        tests/test_rescue.cpp tests/test_clone.cpp tests/test_system.cpp
        tests/test_catalog.cpp tests/test_fuzz.cpp tests/test_health.cpp tests/test_jobs.cpp)
    target_link_libraries(diskforge-selftest PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-selftest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-preview tests/preview.cpp)
    diskforge_add_data(diskforge-preview)
    target_link_libraries(diskforge-preview PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-preview PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    qt_add_executable(diskforge-uitest tests/uitest.cpp)
    diskforge_add_data(diskforge-uitest)
    target_link_libraries(diskforge-uitest PRIVATE diskforge-ui)
    target_compile_definitions(diskforge-uitest PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
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
# Disk Cleanup starts these to shrink the journal and the package cache; see the files for why they're units.
# Off for Flatpak and AppImage builds, which can't add system units.
option(DISKFORGE_SYSTEMD_UNIT "Install the journal cleanup unit" ON)
if(DISKFORGE_SYSTEMD_UNIT)
    pkg_get_variable(SYSTEMD_UNIT_DIR systemd systemd_system_unit_dir)
    if(NOT SYSTEMD_UNIT_DIR)
        set(SYSTEMD_UNIT_DIR ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
    endif()
    install(FILES data/diskforge-journal-vacuum.service data/diskforge-paccache-uninstalled.service DESTINATION ${SYSTEMD_UNIT_DIR})
endif()
