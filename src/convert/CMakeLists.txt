# SPDX-License-Identifier: GPL-3.0-or-later
# convert/: tags, transcoding, artwork, verification, the quality advisor.
# Contract: docs/contracts/media.md; design: DESIGN.md next to this file.

find_package(taglib CONFIG REQUIRED) # 2.3.2 from the overlay port; the build checks the version (detail/TagLibUtil.h)

qt_add_library(selecta_convert STATIC
    MetadataModel.h MetadataModel.cpp
    TagReader.h TagReader.cpp
    TagPolicy.h TagPolicy.cpp
    TagWriter.h TagWriter.cpp
    detail/TagLibUtil.h detail/TagLibUtil.cpp)
add_library(Selecta::Convert ALIAS selecta_convert)
# Qt Gui for QImage/QImageReader (artwork); never Widgets.
target_link_libraries(selecta_convert PUBLIC selecta_audio Qt6::Gui PRIVATE TagLib::tag)
selecta_set_warnings(selecta_convert)
