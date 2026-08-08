cmake_minimum_required(VERSION 3.21)
project(Aerogram VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick QuickControls2 Concurrent)
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
    src/core/Types.h
    src/core/crypto/MasterKeyManager.h
    src/core/plugin/BackendPlugin.h
    src/core/plugin/Capabilities.h
    src/core/plugin/MockBackend.h
    src/core/plugin/DeltaChatBackend.h
    src/core/imap/CurlTransport.h
    src/core/imap/MimeParser.h
    src/core/imap/MessageStore.h
    src/core/imap/MetadataIndex.h
    src/core/imap/ImapBackend.h
    src/core/ipc/IpcServer.h
)

qt_add_qml_module(aerogram
    URI Aerogram
    VERSION 0.1
    QML_FILES
        ui/main.qml
        ui/views/EmailInboxView.qml
        ui/views/ChatView.qml
        ui/views/SettingsView.qml
        ui/components/Sidebar.qml
        ui/components/AddAccountDialog.qml
        ui/components/LockOverlay.qml
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
    KF6::Kirigami
    CURL::libcurl
    ${SQLCIPHER_LIBRARIES}
    ${SODIUM_LIBRARIES}
)
