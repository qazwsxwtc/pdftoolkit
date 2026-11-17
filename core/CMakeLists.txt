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

    # OCR：Windows 10/11 自带的 Windows.Media.Ocr（离线、无模型文件、带词级矩形）。
    # 头文件在 Windows SDK 的 cppwinrt 目录里，默认不在 include 路径上，得单独加；
    # 运行期还需要 WindowsApp.lib 提供 WinRT 激活桩。
    target_sources(pdfword_core PRIVATE src/ocr_windows.cpp)
    # Windows SDK 未必装在系统盘默认位置（本机就在 D:\Windows Kits），所以按候选根目录挨个试，
    # 取最新一个带 cppwinrt\winrt\base.h 的版本目录。
    # 注：CMake 不能写 $ENV{ProgramFiles(x86)}（括号在变量名里非法），这里用 SystemDrive 拼。
    set(_pw_sdk_roots
        "$ENV{WindowsSdkDir}/Include"
        "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/Include"
        "$ENV{SystemDrive}/Program Files/Windows Kits/10/Include"
        "D:/Windows Kits/10/Include"
    )
    set(_pw_cppwinrt "")
    foreach(_root IN LISTS _pw_sdk_roots)
        if(_root AND EXISTS "${_root}")
            file(GLOB _pw_ver_dirs "${_root}/10.*")
            list(SORT _pw_ver_dirs)
            list(REVERSE _pw_ver_dirs)
            foreach(_ver IN LISTS _pw_ver_dirs)
                if(EXISTS "${_ver}/cppwinrt/winrt/base.h")
                    set(_pw_cppwinrt "${_ver}/cppwinrt")
                    break()
                endif()
            endforeach()
        endif()
        if(_pw_cppwinrt)
            break()
        endif()
    endforeach()
    if(EXISTS "${_pw_cppwinrt}/winrt/base.h")
        target_include_directories(pdfword_core PRIVATE "${_pw_cppwinrt}")
        target_compile_definitions(pdfword_core PRIVATE PDFWORD_HAVE_WINRT_OCR=1)
        target_link_libraries(pdfword_core PRIVATE WindowsApp)
        message(STATUS "OCR: 使用 Windows 内置 OCR（cppwinrt=${_pw_cppwinrt}）")
    else()
        message(WARNING "OCR: 没找到 Windows SDK 的 cppwinrt 目录，跳过 Windows OCR")
    endif()
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
