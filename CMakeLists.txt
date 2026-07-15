cmake_minimum_required(VERSION 3.21)
project(Aerogram VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Quick QuickControls2)
find_package(KF6Kirigami REQUIRED)

qt_add_executable(aerogram
    src/main.cpp
    src/controllers/AccountController.h
    src/controllers/AccountController.cpp
    src/models/MessageListModel.h
    src/models/MessageListModel.cpp
    src/models/ChatListModel.h
    src/models/ChatListModel.cpp
    src/core/plugin/BackendPlugin.h
    src/core/plugin/MockBackend.h
)

qt_add_qml_module(aerogram
    URI Aerogram
    VERSION 0.1
    QML_FILES
        ui/main.qml
        ui/views/EmailInboxView.qml
        ui/views/ChatView.qml
        ui/components/Sidebar.qml
)

target_include_directories(aerogram PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(aerogram PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    KF6::Kirigami
)
