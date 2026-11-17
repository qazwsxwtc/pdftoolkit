set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

# 主程序和冒烟测试共用同一份界面源码清单（五个文档级工具是各自独立的对话框）
set(PDFWORD_UI_SOURCES
    MainWindow.cpp
    MainWindow.h
    PdfViewWidget.cpp
    PdfViewWidget.h
    PageEditorDialog.cpp
    PageEditorDialog.h
    ConvertDialog.cpp
    ConvertDialog.h
    MergeDialog.cpp
    MergeDialog.h
    ExtractDialog.cpp
    ExtractDialog.h
    MetadataDialog.cpp
    MetadataDialog.h
    SecurityDialog.cpp
    SecurityDialog.h
    CompressDialog.cpp
    CompressDialog.h
    WatermarkDialog.cpp
    WatermarkDialog.h
    ImageToPdfDialog.cpp
    ImageToPdfDialog.h
    OcrDialog.cpp
    OcrDialog.h
    EditDialogs.cpp
    EditDialogs.h
    DropListWidget.cpp
    DropListWidget.h
    DialogCommon.h
)

add_executable(pdfword_ui WIN32
    main.cpp
    ${PDFWORD_UI_SOURCES}
)

target_link_libraries(pdfword_ui PRIVATE Qt6::Widgets Qt6::Concurrent pdfword_core)

# 界面冒烟测试：真实构造主窗口与各对话框，验证菜单/停靠/校验逻辑不崩。
# 默认不编；要跑时加 -DPDFWORD_BUILD_UI_SMOKE=ON 并构建 pdfword_ui_smoke。
option(PDFWORD_BUILD_UI_SMOKE "构建 Qt 界面的冒烟测试" OFF)
if(PDFWORD_BUILD_UI_SMOKE)
    add_executable(pdfword_ui_smoke
        ui_smoke.cpp
        ${PDFWORD_UI_SOURCES}
    )
    target_link_libraries(pdfword_ui_smoke PRIVATE Qt6::Widgets Qt6::Concurrent pdfword_core)
    if(MSVC)
        target_compile_options(pdfword_ui_smoke PRIVATE /utf-8)
    endif()
endif()

if(MSVC)
    target_compile_options(pdfword_ui PRIVATE /utf-8)
endif()

# 用 windeployqt 收集 Qt 运行库（构建后可选执行一次，脚本见 scripts/deploy.ps1）
