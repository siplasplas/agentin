find_package(Qt6 REQUIRED COMPONENTS Test)

# Helper macro to reduce boilerplate for each test target.
function(diffcore_add_test test_name)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name}
        PRIVATE
            diffcore::diffcore
            Qt6::Test
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

diffcore_add_test(test_engine)
diffcore_add_test(test_interner)
