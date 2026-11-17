cmake_minimum_required(VERSION 3.21)
project(PdfWord VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 顶层开关。core 永远不依赖 Qt，所以即使不装 Qt 也能单独构建 CLI / C 库。
# ---------------------------------------------------------------------------
option(PDFWORD_BUILD_UI "构建 Qt 界面" ON)
option(PDFWORD_BUILD_CLI "构建命令行工具" ON)
option(PDFWORD_BUILD_CAPI "构建 C ABI 动态库（供 web 壳调用）" ON)
option(PDFWORD_BUILD_TESTS "构建测试" ON)
option(PDFWORD_WITH_MUPDF "链接 MuPDF 实现真实 PDF 能力" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /MP /W3)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
                            _WIN32_WINNT=0x0A00)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    # Debug/Release 都落到 bin/ 下同一层，省得拷 DLL 时找错目录
    foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER ${cfg} CFG)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
    endforeach()
endif()

add_subdirectory(core)

if(PDFWORD_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(PDFWORD_BUILD_UI)
    add_subdirectory(ui)
endif()

if(PDFWORD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
