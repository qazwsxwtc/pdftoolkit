add_executable(pdfword_cli main.cpp)
target_link_libraries(pdfword_cli PRIVATE pdfword_core)
if(MSVC)
    target_compile_options(pdfword_cli PRIVATE /utf-8)
endif()

# 配置模板跟 exe 放一起，改一份就能用（不用重新编译）
if(EXISTS ${PROJECT_SOURCE_DIR}/config/pdfword.ini.example)
    add_custom_command(TARGET pdfword_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${PROJECT_SOURCE_DIR}/config/pdfword.ini.example
                $<TARGET_FILE_DIR:pdfword_cli>/pdfword.ini.example)
endif()
