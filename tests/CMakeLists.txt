add_executable(pdfword_tests smoke_test.cpp)
target_link_libraries(pdfword_tests PRIVATE pdfword_core)
add_test(NAME pdfword_smoke COMMAND pdfword_tests)
