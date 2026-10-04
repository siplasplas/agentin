cmake_minimum_required(VERSION 3.24)
project(agentin VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# Installs for the current user unless a prefix is given, so that no root rights are needed.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Installation prefix" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets)
# libgit2 reads the Git baseline of the files a turn changes.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
# The terminal below the chat; qtermwidget is GPL-2.0-or-later, which is why agentin is under the GPL (3.0 only).
find_package(qtermwidget6 REQUIRED)
configure_file(claude/bridge.py ${CMAKE_CURRENT_BINARY_DIR}/claude_bridge.py COPYONLY)

# backward-cpp writes a stack trace after a crash; it declares compatibility with older CMake releases.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
include(FetchContent)
set(backward_exclude)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(backward_exclude EXCLUDE_FROM_ALL)
endif()
# QxFileDialog and MruTabWidget come from qt-extra (github.com/siplasplas/qt-extra). An installed qt-extra 2.1
# or later is used; otherwise CMake fetches the tag below and builds it into agentin, so nothing has to be
# installed first. Version 2.1 adds optional file metadata and the chooser directory getter.
FetchContent_Declare(
    qt-extra
    GIT_REPOSITORY https://github.com/siplasplas/qt-extra.git
    GIT_TAG v2.3.0
    GIT_SHALLOW TRUE
    # Only built into agentin; its own install rules would add its library, headers and CMake package.
    ${backward_exclude}
    # The rest of the line goes to find_package(), so this comes last.
    FIND_PACKAGE_ARGS 2.1
)
FetchContent_MakeAvailable(qt-extra)
# The changes window colours the code of a diff with qcodeedit's reader of Kate syntax definitions
# (github.com/siplasplas/qcodeedit, LGPL-3.0-or-later). An installed qcodeedit-kate 1.4 is used; otherwise the
# tag below is fetched and built into agentin, without its demo, tests and downloader. It builds as C++20 in
# its own directory; the headers agentin includes are C++17.
find_package(qcodeedit-kate 1.4 QUIET)
if(NOT TARGET qcodeedit::kate)
    set(QCE_BUILD_DEMO OFF)
    set(QCE_BUILD_TESTS OFF)
    set(QCE_BUILD_KATE ON)
    set(QCE_BUILD_KATE_DOWNLOADER OFF)
    FetchContent_Declare(
        qcodeedit
        GIT_REPOSITORY https://github.com/siplasplas/qcodeedit.git
        GIT_TAG v1.4.1
        GIT_SHALLOW TRUE
        ${backward_exclude}
    )
    FetchContent_MakeAvailable(qcodeedit)
endif()
FetchContent_Declare(
    backward
    GIT_REPOSITORY https://github.com/bombela/backward-cpp.git
    GIT_TAG 0bfd0a07a61551413ccd2ab9a9099af3bad40681
    GIT_SHALLOW TRUE
    # Only built into agentin; its own install rules would add a header and a CMake package.
    ${backward_exclude}
)
FetchContent_MakeAvailable(backward)

add_executable(agentin
    src/main.cpp
    src/CodexLocator.cpp
    src/CodexLocator.h
    src/AgentBackend.h
    src/CommandApproval.cpp
    src/CommandApproval.h
    src/shell/BashParser.cpp
    src/shell/CommandCatalog.cpp
    src/shell/CommandCatalog.h
    src/shell/ShellAst.h
    src/shell/ShellEvaluator.cpp
    src/shell/ShellEvaluator.h
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
    src/DiffHighlighter.cpp
    src/DiffHighlighter.h
    src/FileOpener.cpp
    src/FileOpener.h
)
target_link_libraries(agentin PRIVATE Qt6::Widgets qt-extra Backward::Object diffcore::diffcore PkgConfig::LIBGIT2 qtermwidget6
                      qcodeedit::kate)
# The window icon; Qt draws it with its SVG image format plugin.
qt_add_resources(agentin icons PREFIX "/" FILES icons/agentin.svg)
target_include_directories(agentin PRIVATE src)
# The version shown by --version and sent to the Codex App Server comes from project().
target_compile_definitions(agentin PRIVATE AGENTIN_VERSION="${PROJECT_VERSION}")
# Keeps the information backward-cpp needs for files and line numbers, also in optimized builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(agentin PRIVATE -g -fno-omit-frame-pointer)
endif()

include(GNUInstallDirs)
install(TARGETS agentin RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The executable looks for the bridge in ../share/agentin relative to its own directory.
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/claude_bridge.py DESTINATION ${CMAKE_INSTALL_DATADIR}/agentin)
# Wayland desktops take the icon of a window from the desktop file named by its application ID.
install(FILES agentin.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES icons/agentin.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Lets the desktop notice the new file at once; the tool may be missing, which is harmless. Icons are found
# without a cache.
install(CODE "execute_process(COMMAND update-desktop-database \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\" ERROR_QUIET)")

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
        src/shell/BashParser.cpp
        src/shell/CommandCatalog.cpp
        src/shell/CommandCatalog.h
        src/shell/ShellAst.h
        src/shell/ShellEvaluator.cpp
        src/shell/ShellEvaluator.h
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
        src/DiffHighlighter.cpp
        src/DiffHighlighter.h
        src/FileOpener.cpp
        src/FileOpener.h
    )
    target_link_libraries(agentin_test PRIVATE Qt6::Widgets Qt6::Test qt-extra diffcore::diffcore PkgConfig::LIBGIT2 qtermwidget6
                          qcodeedit::kate)
    target_include_directories(agentin_test PRIVATE src)
    add_test(NAME agentin_test COMMAND agentin_test)
    # Decisions about shell command lines, against a corpus of real commands; no window, milliseconds.
    add_executable(shell_approval_test tests/ShellApprovalTest.cpp src/CommandApproval.cpp src/CommandApproval.h
        src/shell/BashParser.cpp src/shell/ShellAst.h src/shell/CommandCatalog.cpp src/shell/CommandCatalog.h
        src/shell/ShellEvaluator.cpp src/shell/ShellEvaluator.h)
    target_link_libraries(shell_approval_test PRIVATE Qt6::Core Qt6::Test)
    target_include_directories(shell_approval_test PRIVATE src)
    target_compile_definitions(shell_approval_test PRIVATE
        SHELL_COMMANDS_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/shell-commands.json")
    add_test(NAME shell_approval_test COMMAND shell_approval_test)
    set_tests_properties(agentin_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(Python3_Interpreter_FOUND)
        add_test(NAME claude_bridge_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ClaudeBridgeTest.py)
    endif()
endif()
