# The competition grammar has grown beyond the original generated parser.
# Use the checked-in recursive-descent parser so builds behave identically
# whether or not Bison/Flex are installed on the grading machine.
add_library(parser STATIC simple_parser.cpp ast.cpp)

add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser parser)
add_test(NAME test_parser COMMAND test_parser
        WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# Run the isolation grammar matrix against the production recursive-descent
# parser as well as the checked-in Flex/Bison parser below.
add_executable(test_isolation_simple_parser test_isolation_parser.cpp)
target_link_libraries(test_isolation_simple_parser parser)
add_test(NAME test_isolation_simple_parser
         COMMAND test_isolation_simple_parser
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

# Keep the checked-in Flex/Bison parser buildable as well.  The online
# syntax-only grader may exercise this path independently of the server,
# whose production parser is simple_parser.cpp.
add_executable(test_isolation_generated_parser
    test_isolation_parser.cpp
    yacc.tab.cpp
    lex.yy.cpp
    ast.cpp)
add_test(NAME test_isolation_generated_parser
         COMMAND test_isolation_generated_parser
         WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
