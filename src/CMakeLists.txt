add_library(tu_pccscc_lib
  analysis/CFGAnalyzer.cpp
  analysis/InputValidator.cpp
  analysis/PredicateAnalyzer.cpp
  utils/ExpressionUtils.cpp
  utils/ConditionExtractor.cpp
  utils/VariableSubstitution.cpp
  export/CompletePathConditionBuilder.cpp
  export/CompletePathConditionJSONExporter.cpp
  export/JSONExporter.cpp
  library/PCLibrary.cpp
  library/PTDLibrary.cpp
  solver/PredicateParser.cpp
  solver/Z3PathConditionSolver.cpp
)

target_compile_features(tu_pccscc_lib PUBLIC cxx_std_17)

target_include_directories(tu_pccscc_lib
  PUBLIC
    ${CMAKE_SOURCE_DIR}/include
  PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

if(LLVM_WITH_Z3)
  target_include_directories(tu_pccscc_lib PRIVATE ${Z3_INCLUDE_DIR})
  target_link_libraries(tu_pccscc_lib PRIVATE ${Z3_LIBRARIES})
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(tu_pccscc main.cpp)

target_compile_features(tu_pccscc PRIVATE cxx_std_17)

target_include_directories(tu_pccscc
  PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

target_link_libraries(tu_pccscc PRIVATE
  clangTooling
  clangFrontend
  clangSerialization
  clangSema
  clangParse
  clangAST
  clangASTMatchers
  clangBasic
  LLVMSupport
  tu_pccscc_lib
)

install(TARGETS tu_pccscc RUNTIME DESTINATION bin)
