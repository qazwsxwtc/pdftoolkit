set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(pdfword_ui WIN32
    main.cpp
    MainWindow.cpp
    MainWindow.h
    PdfViewWidget.cpp
    PdfViewWidget.h
    PageEditorDialog.cpp
    PageEditorDialog.h
    DocumentToolsDialog.cpp
    DocumentToolsDialog.h
    ConvertDialog.cpp
    ConvertDialog.h
    MergeDialog.cpp
    MergeDialog.h
    ExtractDialog.cpp
    ExtractDialog.h
)

target_link_libraries(pdfword_ui PRIVATE Qt6::Widgets pdfword_core)

if(MSVC)
    target_compile_options(pdfword_ui PRIVATE /utf-8)
endif()

# 用 windeployqt 收集 Qt 运行库（构建后可选执行一次，脚本见 scripts/deploy.ps1）
