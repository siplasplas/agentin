# libdiffcore, taken from diffmerge (commit 486dce1) and maintained here as part of agentin.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core)

set(DIFFCORE_PUBLIC_HEADERS
    include/diffcore/DiffTypes.h
    include/diffcore/LineInterner.h
    include/diffcore/DiffEngine.h
    include/diffcore/SliderHeuristics.h
)

set(DIFFCORE_SOURCES
    src/LineInterner.cpp
    src/DiffEngine.cpp
    src/SliderHeuristics.cpp
)

set(DIFFCORE_INTERNAL_HEADERS
    src/internal/Diff.h
    src/internal/Structs.h
)

add_library(diffcore STATIC
    ${DIFFCORE_SOURCES}
    ${DIFFCORE_PUBLIC_HEADERS}
    ${DIFFCORE_INTERNAL_HEADERS}
)

# Namespaced alias so consumers can write diffcore::diffcore.
add_library(diffcore::diffcore ALIAS diffcore)

target_include_directories(diffcore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(diffcore PUBLIC Qt6::Core)

# The tests are fast and offline, so they join agentin's suite.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
