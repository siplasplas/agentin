cmake_minimum_required(VERSION 3.16)
project(agentin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
# QxFileDialog and MruTabWidget come from the qt-extra library (source: /home/andrzej/wazne/gitmy/qt-extra),
# installed to the system. Version 2.1 adds optional file metadata and the chooser directory getter.
find_package(qt-extra 2.1 REQUIRED)
# libgit2 reads the Git baseline of the files a turn changes.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
configure_file(claude/bridge.py ${CMAKE_CURRENT_BINARY_DIR}/claude_bridge.py COPYONLY)

# backward-cpp writes a stack trace after a crash; it declares compatibility with older CMake releases.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
include(FetchContent)
FetchContent_Declare(
    backward
    GIT_REPOSITORY https://github.com/bombela/backward-cpp.git
    GIT_TAG 0bfd0a07a61551413ccd2ab9a9099af3bad40681
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(backward)

add_executable(agentin
    src/main.cpp
    src/CodexLocator.cpp
    src/CodexLocator.h
    src/AgentBackend.h
    src/CommandApproval.cpp
    src/CommandApproval.h
    src/ApprovalRules.cpp
    src/ApprovalRules.h
    src/ChatView.cpp
    src/ChatView.h
    src/ChatTab.cpp
    src/ChatTab.h
    src/CodexAgent.cpp
    src/CodexAgent.h
    src/CodexConnection.cpp
    src/CodexConnection.h
    src/ClaudeAgent.cpp
    src/ClaudeAgent.h
    src/GeminiAgent.cpp
    src/GeminiAgent.h
    src/AntigravityAgent.cpp
    src/AntigravityAgent.h
    src/ConversationIndex.cpp
    src/ConversationIndex.h
    src/ProcessLocks.cpp
    src/ProcessLocks.h
    src/TurnLocks.cpp
    src/TurnLocks.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MessageInput.cpp
    src/MessageInput.h
    src/UsageLimitsPanel.cpp
    src/UsageLimitsPanel.h
    src/Notifier.cpp
    src/Notifier.h
    src/LineChanges.cpp
    src/LineChanges.h
    src/GitBaseline.cpp
    src/GitBaseline.h
    src/ChangeTracker.cpp
    src/ChangeTracker.h
    src/ChangesWindow.cpp
    src/ChangesWindow.h
)
target_link_libraries(agentin PRIVATE Qt6::Widgets qt-extra Backward::Object diffcore::diffcore PkgConfig::LIBGIT2)
target_include_directories(agentin PRIVATE src)
# Keeps the information backward-cpp needs for files and line numbers, also in optimized builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(agentin PRIVATE -g -fno-omit-frame-pointer)
endif()

include(GNUInstallDirs)
install(TARGETS agentin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The executable looks for the bridge in ../share/agentin relative to its own directory.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/claude_bridge.py DESTINATION ${CMAKE_INSTALL_DATADIR}/agentin)

option(BUILD_TESTING "Build tests" OFF)
include(CTest)
# The O(NP) line diff, part of agentin's sources.
add_subdirectory(libdiffcore)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    find_package(Python3 COMPONENTS Interpreter)
    add_executable(agentin_test
        tests/MainWindowTest.cpp
        src/CodexLocator.cpp
        src/CodexLocator.h
        src/AgentBackend.h
        src/CommandApproval.cpp
        src/CommandApproval.h
        src/ApprovalRules.cpp
        src/ApprovalRules.h
        src/ChatView.cpp
        src/ChatView.h
        src/ChatTab.cpp
        src/ChatTab.h
        src/CodexAgent.cpp
        src/CodexAgent.h
        src/CodexConnection.cpp
        src/CodexConnection.h
        src/ClaudeAgent.cpp
        src/ClaudeAgent.h
        src/GeminiAgent.cpp
        src/GeminiAgent.h
        src/AntigravityAgent.cpp
        src/AntigravityAgent.h
        src/ConversationIndex.cpp
        src/ConversationIndex.h
        src/ProcessLocks.cpp
        src/ProcessLocks.h
        src/TurnLocks.cpp
        src/TurnLocks.h
        src/MainWindow.cpp
        src/MainWindow.h
        src/MessageInput.cpp
        src/MessageInput.h
        src/UsageLimitsPanel.cpp
        src/UsageLimitsPanel.h
        src/Notifier.cpp
        src/Notifier.h
        src/LineChanges.cpp
        src/LineChanges.h
        src/GitBaseline.cpp
        src/GitBaseline.h
        src/ChangeTracker.cpp
        src/ChangeTracker.h
        src/ChangesWindow.cpp
        src/ChangesWindow.h
    )
    target_link_libraries(agentin_test PRIVATE Qt6::Widgets Qt6::Test qt-extra diffcore::diffcore PkgConfig::LIBGIT2)
    target_include_directories(agentin_test PRIVATE src)
    add_test(NAME agentin_test COMMAND agentin_test)
    set_tests_properties(agentin_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(Python3_Interpreter_FOUND)
        add_test(NAME claude_bridge_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ClaudeBridgeTest.py)
    endif()
endif()
