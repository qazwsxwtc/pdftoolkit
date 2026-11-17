# core：纯 C++17，不依赖 Qt，不依赖任何第三方库（MuPDF 可选）
add_library(pdfword_core STATIC
    src/result.cpp
    src/config.cpp
    src/office_runner.cpp
    src/page_selection.cpp
    src/zip_writer.cpp
    src/docx_writer.cpp
    src/pdf_backend_factory.cpp
    src/pdf_backend_stub.cpp
    src/converter.cpp
    src/job.cpp
)

target_include_directories(pdfword_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(PDFWORD_WITH_MUPDF)
    target_sources(pdfword_core PRIVATE src/pdf_backend_mupdf.cpp)
    list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
    # 优先用 vcpkg 导出的目标：它会把 freetype / harfbuzz / jbig2dec / openjpeg /
    # libjpeg-turbo / zlib / gumbo 这些传递依赖一起带上，静态库才不会缺符号。
    find_package(unofficial-libmupdf CONFIG QUIET)
    if(TARGET unofficial::libmupdf::libmupdf)
        set(PW_MUPDF_TARGET unofficial::libmupdf::libmupdf)
        message(STATUS "MuPDF: 使用 vcpkg 目标 unofficial::libmupdf::libmupdf")
    else()
        find_package(MuPDF REQUIRED)
        set(PW_MUPDF_TARGET MuPDF::mupdf)
        message(STATUS "MuPDF: 使用 FindMuPDF 找到的 ${MUPDF_LIBRARY}")
    endif()
    target_compile_definitions(pdfword_core PRIVATE PDFWORD_HAVE_MUPDF=1)
    target_link_libraries(pdfword_core PUBLIC ${PW_MUPDF_TARGET})
endif()

if(WIN32)
    # 注册表探测 LibreOffice 需要 advapi32
    target_link_libraries(pdfword_core PRIVATE advapi32)
endif()

# ---------------------------------------------------------------------------
# C ABI：web 壳（HTTP 服务 / WebView / Go、C#、Python FFI）只需要这一个 DLL
# ---------------------------------------------------------------------------
if(PDFWORD_BUILD_CAPI)
    add_library(pdfword_c SHARED capi/pdfword_c.cpp)
    set_target_properties(pdfword_c PROPERTIES OUTPUT_NAME "pdfword")
    target_include_directories(pdfword_c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/capi)
    target_link_libraries(pdfword_c PRIVATE pdfword_core)
    if(NOT WIN32)
        set_target_properties(pdfword_c PROPERTIES CXX_VISIBILITY_PRESET hidden)
    endif()
endif()
