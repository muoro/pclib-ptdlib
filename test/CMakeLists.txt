# Only build tests if LLVM's GTest libraries are available.
if(PREDICATES_GTEST_AVAILABLE)
    message(STATUS "Building test targets with LLVM GTest")

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
    add_executable(test_runner
        main.cpp
        test.cpp
        incremental_core_test.cpp
        library_roundtrip_test.cpp
    )

    target_link_libraries(test_runner
      PRIVATE
        tu_pccscc_lib
        ${PREDICATES_LLVM_GTEST_LIB}
        clangTooling
        clangBasic
        clangAST
        LLVMSupport
    )

    target_include_directories(test_runner
      PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CLANG_INCLUDE_DIRS}
        ${LLVM_INCLUDE_DIRS}
        ${PREDICATES_GTEST_INCLUDE_DIR}
    )

    include(GoogleTest)
    gtest_discover_tests(test_runner)
else()
    message(STATUS "Unit tests disabled - LLVM GTest not available")
endif()


find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME shared_model_headers
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/shared_model_headers_test.py
            --cxx ${CMAKE_CXX_COMPILER}
            --source-root ${CMAKE_SOURCE_DIR})
    set_tests_properties(shared_model_headers PROPERTIES TIMEOUT 180)
    add_test(NAME paper_experiments
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/paper_experiments_test.py
            --source-root ${CMAKE_SOURCE_DIR})
    set_tests_properties(paper_experiments PROPERTIES TIMEOUT 60)
    if(UNIX)
        add_test(NAME release_scripts
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/release_scripts_test.py
                --source-root ${CMAKE_SOURCE_DIR})
        set_tests_properties(release_scripts PROPERTIES TIMEOUT 60)
    endif()
endif()

if(Python3_Interpreter_FOUND AND LLVM_WITH_Z3)
    add_test(NAME library_integration
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/library_integration.py
            --tool $<TARGET_FILE:tu_pccscc>
            --source-root ${CMAKE_SOURCE_DIR}
            --cc ${CMAKE_C_COMPILER})
    set_tests_properties(library_integration PROPERTIES TIMEOUT 180)
endif()
