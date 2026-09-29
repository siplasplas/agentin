cmake_minimum_required(VERSION 3.16)
project(agentdeskt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(agentdeskt
    src/main.cpp
    src/CodexLocator.cpp
    src/CodexLocator.h
    src/MainWindow.cpp
    src/MainWindow.h
)
target_link_libraries(agentdeskt PRIVATE Qt6::Widgets)
target_include_directories(agentdeskt PRIVATE src)

option(BUILD_TESTING "Build tests" OFF)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(agentdeskt_test
        tests/MainWindowTest.cpp
        src/CodexLocator.cpp
        src/CodexLocator.h
        src/MainWindow.cpp
        src/MainWindow.h
    )
    target_link_libraries(agentdeskt_test PRIVATE Qt6::Widgets Qt6::Test)
    target_include_directories(agentdeskt_test PRIVATE src)
    add_test(NAME agentdeskt_test COMMAND agentdeskt_test)
    set_tests_properties(agentdeskt_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
