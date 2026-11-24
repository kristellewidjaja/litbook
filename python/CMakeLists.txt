# The Python module `litbook`, a thin pybind11 layer over the blocking order client and the
# feed book in src/client/. All the logic stays in C++, where the GoogleTest tests check it
# under every sanitizer, and this file only turns C++ values into Python ones.
pybind11_add_module(litbook_python MODULE litbook_module.cpp)
set_target_properties(litbook_python PROPERTIES
    OUTPUT_NAME litbook
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/python)
target_link_libraries(litbook_python PRIVATE litbook_pic litbook::warnings)

# The Python tests start a real litbook_gateway and talk to it through the module. Each
# file is its own ctest test, so they can run in parallel. Every gateway listens on a port
# the kernel picks and sends its feed to ports the test's own receiver picked, so parallel
# runs never share a port.
if(LITBOOK_BUILD_TESTS)
    foreach(name order_client feed_book)
        add_test(NAME Python.${name}
            COMMAND Python::Interpreter -m unittest discover
                -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_${name}.py -v)
        set_tests_properties(Python.${name} PROPERTIES
            TIMEOUT 120
            ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:litbook_python>;PYTHONDONTWRITEBYTECODE=1;LITBOOK_GATEWAY=$<TARGET_FILE:litbook_gateway>")
    endforeach()
endif()
