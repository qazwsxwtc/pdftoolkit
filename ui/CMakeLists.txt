set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

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
    AppIcon.cpp
    AppIcon.h
    DialogCommon.h
)

# 资源：logo（png 给界面用，ico 给窗口/托盘多尺寸用）+ Windows exe 图标
# rc 里的相对路径 "pdftoolkit.ico" 靠下面这条 include 目录解析
set(PDFWORD_UI_RESOURCES
    resources/icons.qrc
    resources/pdftoolkit.rc
)

add_executable(pdfword_ui WIN32
    main.cpp
    ${PDFWORD_UI_SOURCES}
    ${PDFWORD_UI_RESOURCES}
)
# rc.exe 不会去源目录找图标，显式把 resources 目录交给它
set_source_files_properties(resources/pdftoolkit.rc PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/resources")
# 产物文件名 = 产品名：pdftoolkit.exe（target 名保持 pdfword_ui，脚本与冒烟引用不用动）
set_target_properties(pdfword_ui PROPERTIES OUTPUT_NAME "pdftoolkit")

target_link_libraries(pdfword_ui PRIVATE Qt6::Widgets Qt6::Concurrent pdfword_core)
# 「关于」窗口里的版本号取自 project(VERSION)，只改 CMakeLists 一处
target_compile_definitions(pdfword_ui PRIVATE PDFWORD_APP_VERSION="${PROJECT_VERSION}")

# 界面冒烟测试：真实构造主窗口与各对话框，验证菜单/停靠/校验逻辑不崩。
# 默认不编；要跑时加 -DPDFWORD_BUILD_UI_SMOKE=ON 并构建 pdfword_ui_smoke。
option(PDFWORD_BUILD_UI_SMOKE "构建 Qt 界面的冒烟测试" OFF)
if(PDFWORD_BUILD_UI_SMOKE)
    add_executable(pdfword_ui_smoke
        ui_smoke.cpp
        ${PDFWORD_UI_SOURCES}
        resources/icons.qrc        # 冒烟也要能取到 logo（不链 rc，冒烟不需要 exe 图标）
    )
    target_link_libraries(pdfword_ui_smoke PRIVATE Qt6::Widgets Qt6::Concurrent pdfword_core)
target_compile_definitions(pdfword_ui_smoke PRIVATE PDFWORD_APP_VERSION="${PROJECT_VERSION}")
    if(MSVC)
        target_compile_options(pdfword_ui_smoke PRIVATE /utf-8)
    endif()
endif()

if(MSVC)
    target_compile_options(pdfword_ui PRIVATE /utf-8)
endif()

# 用 windeployqt 收集 Qt 运行库（构建后可选执行一次，脚本见 scripts/deploy.ps1）
