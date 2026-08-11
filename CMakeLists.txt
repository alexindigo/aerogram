cmake_minimum_required(VERSION 3.21)
project(Aerogram VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick QuickControls2 Concurrent Multimedia)
find_package(ZXing REQUIRED)
find_package(KF6Kirigami REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLCIPHER REQUIRED sqlcipher)
pkg_check_modules(SODIUM REQUIRED libsodium)

qt_add_executable(aerogram
    src/main.cpp
    src/controllers/AccountController.h
    src/controllers/AccountController.cpp
    src/models/MessageListModel.h
    src/models/MessageListModel.cpp
    src/models/ConversationListModel.h
    src/models/ConversationListModel.cpp
    src/models/AccountListModel.h
    src/models/AccountListModel.cpp
    src/core/Types.h
    src/core/Account.h
    src/core/crypto/MasterKeyManager.h
    src/core/crypto/AccountStore.h
    src/core/plugin/BackendPlugin.h
    src/core/plugin/BackendRegistry.h
    src/core/plugin/Capabilities.h
    src/core/plugin/MockBackend.h
    src/core/plugin/DeltaChatBackend.h
    src/core/imap/CurlTransport.h
    src/core/imap/MimeParser.h
    src/core/imap/MessageStore.h
    src/core/imap/MetadataIndex.h
    src/core/imap/ImapBackend.h
    src/core/ipc/IpcServer.h
    src/core/proton/ProtonBackend.h
    src/core/proton/ProtonBackend.cpp
    src/ui/QrCodeHelper.h
)

# --- Proton core (Rust staticlib) -------------------------------------
# Built by cargo from proton-core/. The .a is fat (debug symbols) but
# only ever rebuilt when Rust sources change.
find_program(CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
if(CARGO)
    set(PROTON_CORE_LIB
        ${CMAKE_SOURCE_DIR}/proton-core/target/debug/libaerogram_proton_core.a)
    # gopenpgp-sys (Proton's OpenPGP) is a Go archive built by its
    # cargo build script into a hashed out/ dir; copy it to a stable
    # path so the C++ link can reference it.
    set(GOPENPGP_LIB
        ${CMAKE_SOURCE_DIR}/proton-core/target/debug/libgopenpgp-sys.a)
    file(GLOB PROTON_CORE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/proton-core/src/*.rs
        ${CMAKE_SOURCE_DIR}/proton-core/Cargo.toml)
    add_custom_command(
        OUTPUT ${PROTON_CORE_LIB} ${GOPENPGP_LIB}
        COMMAND ${CARGO} build
        COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/proton-core/copy-gopenpgp.sh
        DEPENDS ${PROTON_CORE_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/proton-core
        COMMENT "cargo build aerogram-proton-core")
    add_custom_target(proton_core ALL DEPENDS ${PROTON_CORE_LIB} ${GOPENPGP_LIB})
    add_library(aerogram_proton_core STATIC IMPORTED GLOBAL)
    set_target_properties(aerogram_proton_core PROPERTIES
        IMPORTED_LOCATION ${PROTON_CORE_LIB})
    add_library(gopenpgp_sys STATIC IMPORTED GLOBAL)
    set_target_properties(gopenpgp_sys PROPERTIES
        IMPORTED_LOCATION ${GOPENPGP_LIB})
    add_dependencies(aerogram_proton_core proton_core)
    set(HAVE_PROTON_CORE TRUE)
else()
    message(WARNING "cargo not found — Proton backend disabled")
endif()

# We intentionally keep .qml files in subdirectories (ui/views,
# ui/components) of one module — set QTP0004 to acknowledge that.
# QTP0001 left at default: NEW would move the resource prefix to
# :/qt/qml/ and our loader path is qrc:/Aerogram/ui/main.qml.
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_qml_module(aerogram
    URI Aerogram
    VERSION 0.1
    QML_FILES
        ui/main.qml
        ui/views/EmailConversationsPanel.qml
        ui/views/EmailMessagesPanel.qml
        ui/views/ChatView.qml
        ui/views/SettingsView.qml
        ui/views/AddAccountPanel.qml
        ui/components/Sidebar.qml
        ui/components/QrScannerDialog.qml
        ui/components/LockOverlay.qml
        ui/components/AeroIcon.qml
        ui/components/IdentityBlock.qml
)

# Alias strips the assets/ path so resources resolve as
# :/icons/tabler/<name>.svg.
foreach(icon plus settings lock paperclip)
    set_source_files_properties(assets/icons/tabler/${icon}.svg
        PROPERTIES QT_RESOURCE_ALIAS ${icon}.svg)
endforeach()

qt_add_resources(aerogram "icons"
    PREFIX "/icons/tabler"
    FILES
        assets/icons/tabler/plus.svg
        assets/icons/tabler/settings.svg
        assets/icons/tabler/lock.svg
        assets/icons/tabler/paperclip.svg
)

target_include_directories(aerogram PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLCIPHER_INCLUDE_DIRS}
    ${SODIUM_INCLUDE_DIRS}
)

target_link_directories(aerogram PRIVATE
    ${SQLCIPHER_LIBRARY_DIRS}
    ${SODIUM_LIBRARY_DIRS}
)

# sqlcipher/sqlite3.h declares sqlite3_key() only under this guard.
target_compile_definitions(aerogram PRIVATE SQLITE_HAS_CODEC)

target_link_libraries(aerogram PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Concurrent
    Qt6::Multimedia
    KF6::Kirigami
    CURL::libcurl
    ZXing::ZXing
    ${SQLCIPHER_LIBRARIES}
    ${SODIUM_LIBRARIES}
)

if(HAVE_PROTON_CORE)
    # Rust staticlib + Go crypto archive; pthread/dl/m for both
    # runtimes, resolv for Go's net resolver.
    target_link_libraries(aerogram PRIVATE aerogram_proton_core gopenpgp_sys
        pthread dl m resolv)
    target_compile_definitions(aerogram PRIVATE AEROGRAM_HAVE_PROTON)
endif()
