cmake_minimum_required(VERSION 3.16)
project(agentdeskt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
# QxFileDialog and MruTabWidget come from the qt-extra library (source: /home/andrzej/wazne/gitmy/qt-extra),
# installed to the system. Version 2 identifies tabs by page and adds the preview tab.
find_package(qt-extra 2 REQUIRED)
configure_file(claude/bridge.py ${CMAKE_CURRENT_BINARY_DIR}/claude_bridge.py COPYONLY)

add_executable(agentdeskt
    src/main.cpp
    src/CodexLocator.cpp
    src/CodexLocator.h
    src/AgentBackend.h
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
    src/Notifier.cpp
    src/Notifier.h
)
target_link_libraries(agentdeskt PRIVATE Qt6::Widgets qt-extra)
target_include_directories(agentdeskt PRIVATE src)

option(BUILD_TESTING "Build tests" OFF)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    find_package(Python3 COMPONENTS Interpreter)
    add_executable(agentdeskt_test
        tests/MainWindowTest.cpp
        src/CodexLocator.cpp
        src/CodexLocator.h
        src/AgentBackend.h
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
        src/Notifier.cpp
        src/Notifier.h
    )
    target_link_libraries(agentdeskt_test PRIVATE Qt6::Widgets Qt6::Test qt-extra)
    target_include_directories(agentdeskt_test PRIVATE src)
    add_test(NAME agentdeskt_test COMMAND agentdeskt_test)
    set_tests_properties(agentdeskt_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(Python3_Interpreter_FOUND)
        add_test(NAME claude_bridge_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ClaudeBridgeTest.py)
    endif()
endif()
