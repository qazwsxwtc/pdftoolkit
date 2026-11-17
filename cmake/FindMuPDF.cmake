# MuPDF 查找模块。
# 支持两种来源：
#   1) 便携/解压的 SDK：<root>/include/mupdf/fitz.h + <root>/lib/mupdf.lib
#   2) vcpkg 安装树（VCPKG_ROOT 已设置时直接命中）
#
# 手动指定：cmake -DPDFWORD_MUPDF_ROOT=D:/sdk/mupdf ...

find_path(MUPDF_INCLUDE_DIR
    NAMES mupdf/fitz.h
    HINTS ${PDFWORD_MUPDF_ROOT} $ENV{PW_MUPDF_ROOT} $ENV{MUPDF_ROOT}
    PATH_SUFFIXES include
)

find_library(MUPDF_LIBRARY
    NAMES mupdf libmupdf
    HINTS ${PDFWORD_MUPDF_ROOT} $ENV{PW_MUPDF_ROOT} $ENV{MUPDF_ROOT}
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MuPDF
    REQUIRED_VARS MUPDF_INCLUDE_DIR MUPDF_LIBRARY
    FAIL_MESSAGE "未找到 MuPDF。请设置 -DPDFWORD_MUPDF_ROOT=<SDK 根目录> 或环境变量 PW_MUPDF_ROOT"
)

if(MuPDF_FOUND AND NOT TARGET MuPDF::mupdf)
    add_library(MuPDF::mupdf UNKNOWN IMPORTED)
    set_target_properties(MuPDF::mupdf PROPERTIES
        IMPORTED_LOCATION "${MUPDF_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MUPDF_INCLUDE_DIR}"
    )
endif()
