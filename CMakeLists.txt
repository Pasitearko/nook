cmake_minimum_required(VERSION 3.25)
project(nook VERSION 0.3.4 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Concurrent Network DBus Test ShaderTools)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
pkg_check_modules(ICU REQUIRED IMPORTED_TARGET icu-i18n icu-uc)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
find_package(SQLite3 REQUIRED)
qt_standard_project_setup(REQUIRES 6.8)
add_library(nook_core STATIC
    src/files.cpp src/files.h src/storage.cpp src/storage.h src/tags.cpp src/tags.h
    src/directorymodel.cpp src/directorymodel.h
    src/operations.cpp src/operations.h src/undorecord.cpp src/undorecord.h
    src/searchindex.cpp src/searchindex.h
    src/preview.cpp src/preview.h src/application.cpp src/application.h
    src/updatechecker.cpp src/updatechecker.h
    src/previewdocuments.cpp src/previewdocuments.h
    src/fileproperties.cpp src/fileproperties.h
    src/diskmounts.cpp src/diskmounts.h src/ntfs.cpp src/ntfs.h
    src/filemanager1.cpp src/filemanager1.h src/filechooser.cpp src/filechooser.h src/appearance.cpp src/appearance.h)
target_link_libraries(nook_core PUBLIC Qt6::Core Qt6::Gui Qt6::Quick Qt6::Concurrent Qt6::Network Qt6::DBus
    PkgConfig::GIO PkgConfig::ICU PkgConfig::LIBARCHIVE SQLite3::SQLite3 nook_archive)
target_include_directories(nook_core PUBLIC src)
target_compile_definitions(nook_core PUBLIC QT_NO_KEYWORDS)
target_compile_options(nook_core PRIVATE -Wall -Wextra -Wpedantic)
# nook解压缩: naming rules and the client of the unprivileged worker that reads and writes packages.
add_library(nook_archive STATIC
    src/archive/archivenames.cpp src/archive/archivenames.h
    src/archive/archivejob.cpp src/archive/archivejob.h)
target_link_libraries(nook_archive PUBLIC Qt6::Core PkgConfig::ICU)
target_include_directories(nook_archive PUBLIC src/archive)
target_compile_definitions(nook_archive PUBLIC QT_NO_KEYWORDS
    PRIVATE NOOK_ARCHIVE_WORKER="${CMAKE_INSTALL_PREFIX}/lib/nook/nook-archive-worker")
target_compile_options(nook_archive PRIVATE -Wall -Wextra -Wpedantic)
add_executable(nook-archive-worker
    src/archive/worker/main.cpp src/archive/worker/worker.h src/archive/worker/channel.cpp
    src/archive/worker/sandbox.cpp src/archive/worker/stage.cpp
    src/archive/worker/libarchivebackend.cpp src/archive/worker/sevenzip.cpp src/archive/worker/create.cpp
    src/archive/worker/modify.cpp)
target_link_libraries(nook-archive-worker PRIVATE nook_archive PkgConfig::LIBARCHIVE)
target_compile_options(nook-archive-worker PRIVATE -Wall -Wextra -Wpedantic)
qt_add_executable(nook src/main.cpp)
# Controls and artwork shared by nook and nook解压缩; each program compiles them into its own
# QML module. They expect their window to provide `theme` (Theme) and `animations` (bool).
set(NOOK_SHARED_QML
    qml/ActionButton.qml qml/Icon.qml qml/NookMenu.qml qml/NookMenuItem.qml qml/NookToolTip.qml
    qml/NookComboBox.qml qml/Spotlight.qml qml/Theme.qml qml/palette.js qml/TextMenu.qml
    qml/TextMenuArea.qml qml/WindowCloseButton.qml qml/NookDialog.qml qml/motion.js qml/Glide.qml
    qml/HoverPlate.qml qml/NookScrollBar.qml)
set(NOOK_ICONS
    qml/icons/folder.svg qml/icons/file.svg qml/icons/image.svg
    qml/icons/code.svg qml/icons/document.svg qml/icons/video.svg qml/icons/music.svg
    qml/icons/home.svg qml/icons/download.svg qml/icons/drive.svg qml/icons/trash.svg
    qml/icons/search.svg qml/icons/settings.svg qml/icons/add.svg qml/icons/close.svg
    qml/icons/back.svg qml/icons/forward.svg qml/icons/up.svg qml/icons/refresh.svg
    qml/icons/cut.svg qml/icons/copy.svg qml/icons/paste.svg qml/icons/rename.svg
    qml/icons/list.svg qml/icons/grid.svg qml/icons/split.svg qml/icons/terminal.svg
    qml/icons/split-single.svg qml/icons/split-vertical.svg qml/icons/split-triple.svg qml/icons/split-quad.svg
    qml/icons/hidden.svg qml/icons/filter.svg qml/icons/calendar.svg qml/icons/undo.svg
    qml/icons/redo.svg qml/icons/select-all.svg qml/icons/select.svg qml/icons/sun.svg
    qml/icons/moon.svg qml/icons/archive.svg qml/icons/lock.svg qml/icons/info.svg
    qml/icons/verify.svg qml/icons/extract.svg qml/icons/compress.svg
    qml/icons/details.svg qml/icons/tag.svg qml/icons/open.svg qml/icons/newtab.svg
    qml/icons/nook.svg)
qt_add_qml_module(nook URI Nook VERSION 1.0
    QML_FILES ${NOOK_SHARED_QML}
        qml/Main.qml qml/FilePane.qml qml/SettingsPanel.qml qml/Sidebar.qml qml/PropertiesDialog.qml
        qml/RecentActivity.qml qml/DatePicker.qml qml/SpringProgress.qml qml/FileChooser.qml
        qml/DetailsPane.qml qml/ResizeGrip.qml qml/ZoomSlider.qml
    RESOURCES ${NOOK_ICONS}
)
# GLSL sources are compiled to portable .qsb (Vulkan/GL/Metal) at build time.
qt_add_shaders(nook "nook-shaders"
    PREFIX "/qt/qml/Nook"
    FILES qml/shaders/spotlight.frag qml/shaders/tint.frag)
target_link_libraries(nook PRIVATE nook_core Qt6::QuickControls2)
# nook解压缩: its own program and identity (local.nook.archive), sharing nook's look and settings.
set(NOOK_ARCHIVE_APP_SOURCES
    src/archive/app/archiveapp.cpp src/archive/app/archiveapp.h
    src/archive/app/archivedocument.cpp src/archive/app/archivedocument.h
    src/archive/app/archivemodel.cpp src/archive/app/archivemodel.h
    src/archive/app/taskqueue.cpp src/archive/app/taskqueue.h
    src/archive/app/passwordstore.cpp src/archive/app/passwordstore.h
    src/archive/app/notifier.cpp src/archive/app/notifier.h
    src/archive/app/filesessions.cpp src/archive/app/filesessions.h
    src/archive/app/defaultapps.cpp src/archive/app/defaultapps.h)
qt_add_executable(nook-archive src/archive/app/main.cpp ${NOOK_ARCHIVE_APP_SOURCES})
qt_add_qml_module(nook-archive URI NookArchive VERSION 1.0
    QML_FILES ${NOOK_SHARED_QML} qml/ArchiveMain.qml qml/ArchiveList.qml
        qml/ArchiveWindow.qml qml/ArchiveField.qml qml/ArchivePasswordField.qml qml/ArchiveCheck.qml
        qml/ArchiveSwitch.qml qml/ArchiveProgress.qml qml/ArchivePasswordDialog.qml qml/ArchiveConflictDialog.qml
        qml/ArchivePrompts.qml qml/ArchiveTaskCard.qml qml/ArchiveTasks.qml qml/ArchiveExtractDialog.qml
        qml/ArchiveSettings.qml qml/ArchiveCompose.qml qml/ArchiveTaskWindow.qml qml/ArchiveDefaultDialog.qml
    RESOURCES ${NOOK_ICONS}
)
qt_add_shaders(nook-archive "nook-archive-shaders"
    PREFIX "/qt/qml/NookArchive"
    FILES qml/shaders/spotlight.frag qml/shaders/tint.frag)
target_include_directories(nook-archive PRIVATE src/archive/app)
target_link_libraries(nook-archive PRIVATE nook_core nook_archive Qt6::QuickControls2 Qt6::DBus PkgConfig::LIBSECRET)
add_dependencies(nook-archive nook-archive-worker)
# Development probe for NTFS volumes (read-only); built, never installed.
add_executable(nook-ntfs-probe tools/ntfs_probe.cpp)
target_link_libraries(nook-ntfs-probe PRIVATE nook_core)
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME mount-helper COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mount_helper_test.py)
    add_test(NAME volume-helper COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/volume_helper_test.py)
    add_test(NAME archive-compare-checks COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_compare_test.py)
    qt_add_executable(nook-directory-features-tests tests/directorymodel_features_test.cpp)
    target_link_libraries(nook-directory-features-tests PRIVATE nook_core Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(nook-directory-features-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME directory-features COMMAND nook-directory-features-tests)
    set_tests_properties(directory-features PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-preview-documents-tests tests/preview_documents_test.cpp)
    target_link_libraries(nook-preview-documents-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME preview-documents COMMAND nook-preview-documents-tests)
    set_tests_properties(preview-documents PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(nook-feature-ui-tests tests/feature_ui_test.cpp)
    target_link_libraries(nook-feature-ui-tests PRIVATE nook_core Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(nook-feature-ui-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME feature-ui COMMAND nook-feature-ui-tests)
    set_tests_properties(feature-ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-viewstate-tests tests/viewstate_test.cpp)
    target_link_libraries(nook-viewstate-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME viewstate COMMAND nook-viewstate-tests)
    set_tests_properties(viewstate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(nook-updatechecker-tests tests/updatechecker_test.cpp)
    target_link_libraries(nook-updatechecker-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME updatechecker COMMAND nook-updatechecker-tests)
    qt_add_executable(nook-tests tests/core_test.cpp)
    target_link_libraries(nook-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME core COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-core.sh $<TARGET_FILE:nook-tests>)
    set_tests_properties(core PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(nook-operations-progress-tests tests/operations_progress_test.cpp)
    target_link_libraries(nook-operations-progress-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME operations-progress COMMAND nook-operations-progress-tests)
    qt_add_executable(nook-undo-tests tests/undo_test.cpp)
    target_link_libraries(nook-undo-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME undo COMMAND nook-undo-tests)
    qt_add_executable(nook-ntfs-tests tests/ntfs_test.cpp tests/ntfs_image.h)
    target_link_libraries(nook-ntfs-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME ntfs COMMAND nook-ntfs-tests)
    qt_add_executable(nook-properties-many-tests tests/properties_many_test.cpp)
    target_link_libraries(nook-properties-many-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME properties-many COMMAND nook-properties-many-tests)
    set_tests_properties(properties-many PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(nook-properties-tests tests/properties_test.cpp)
    target_link_libraries(nook-properties-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME properties COMMAND nook-properties-tests)
    set_tests_properties(properties PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(nook-thumbnail-tests tests/thumbnail_test.cpp)
    target_link_libraries(nook-thumbnail-tests PRIVATE nook_core Qt6::Test)
    add_test(NAME thumbnails COMMAND nook-thumbnail-tests)
    set_tests_properties(thumbnails PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-followup-ui-tests tests/followup_ui_test.cpp)
    target_link_libraries(nook-followup-ui-tests PRIVATE nook_core Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(nook-followup-ui-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOOK_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME followup-ui COMMAND nook-followup-ui-tests)
    set_tests_properties(followup-ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-chooser-tests tests/chooser_test.cpp)
    target_link_libraries(nook-chooser-tests PRIVATE nook_core Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(nook-chooser-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOOK_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME chooser COMMAND nook-chooser-tests)
    set_tests_properties(chooser PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-ui-tests tests/ui_test.cpp)
    target_link_libraries(nook-ui-tests PRIVATE nook_core Qt6::Test Qt6::QuickControls2)
    target_compile_definitions(nook-ui-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOOK_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME ui COMMAND nook-ui-tests)
    set_tests_properties(ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    qt_add_executable(nook-archive-tests tests/archive_test.cpp)
    target_link_libraries(nook-archive-tests PRIVATE nook_archive Qt6::Test PkgConfig::LIBARCHIVE)
    target_compile_definitions(nook-archive-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(nook-archive-tests nook-archive-worker)
    add_test(NAME archive COMMAND nook-archive-tests)
    set_tests_properties(archive PROPERTIES ENVIRONMENT "NOOK_ARCHIVE_WORKER=$<TARGET_FILE:nook-archive-worker>;LANG=zh_CN.UTF-8")
    # Its own process: the associations it changes live in a fixture set up before GIO starts.
    qt_add_executable(nook-archive-defaults-tests tests/archive_defaults_test.cpp src/archive/app/defaultapps.cpp src/archive/app/defaultapps.h)
    target_include_directories(nook-archive-defaults-tests PRIVATE src/archive/app)
    target_link_libraries(nook-archive-defaults-tests PRIVATE Qt6::Test PkgConfig::GIO)
    target_compile_definitions(nook-archive-defaults-tests PRIVATE QT_NO_KEYWORDS NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOOK_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME archive-defaults COMMAND nook-archive-defaults-tests)
    qt_add_executable(nook-archive-app-tests tests/archive_app_test.cpp ${NOOK_ARCHIVE_APP_SOURCES})
    target_include_directories(nook-archive-app-tests PRIVATE src/archive/app)
    target_link_libraries(nook-archive-app-tests PRIVATE nook_core nook_archive Qt6::Test Qt6::QuickControls2 Qt6::DBus PkgConfig::LIBARCHIVE PkgConfig::LIBSECRET)
    target_compile_definitions(nook-archive-app-tests PRIVATE NOOK_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOOK_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")
    add_dependencies(nook-archive-app-tests nook-archive-worker)
    add_test(NAME archive-app COMMAND nook-archive-app-tests)
    set_tests_properties(archive-app PROPERTIES ENVIRONMENT "NOOK_ARCHIVE_WORKER=$<TARGET_FILE:nook-archive-worker>;LANG=zh_CN.UTF-8;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
include(GNUInstallDirs)
install(TARGETS nook nook-archive RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/local.nook.desktop packaging/local.nook.chooser.desktop packaging/local.nook.archive.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# The file chooser behind xdg-desktop-portal; portals.conf decides which desktops use it.
install(FILES packaging/nook.portal DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES packaging/org.freedesktop.impl.portal.desktop.nook.service DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
# The pkexec policy intentionally names an absolute, root-owned installed helper.
install(PROGRAMS packaging/nook-mount-helper DESTINATION lib/nook)
install(PROGRAMS packaging/nook-volume-helper packaging/nook-volume-setup DESTINATION lib/nook)
install(TARGETS nook-archive-worker RUNTIME DESTINATION lib/nook)
install(FILES packaging/local.nook.mount.policy packaging/local.nook.volume.policy
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
install(FILES qml/icons/nook.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES qml/icons/archive.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME nook-archive.svg)
install(FILES LICENSE THIRD_PARTY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/nook)
install(FILES README.md LICENSE THIRD_PARTY.md packaging/niri.example.kdl DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nook)
install(FILES docs/VALIDATION.md docs/AUTOMOUNT-DESIGN.md docs/INPUT-METHOD.md docs/ARCHIVE-DESIGN.md docs/ARCHIVE-WORKER.md docs/ARCHIVE-ACCEPTANCE.md docs/UNDO.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nook/docs)
install(DIRECTORY docs/screenshots DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nook/docs)
