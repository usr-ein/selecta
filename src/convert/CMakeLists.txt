# SPDX-License-Identifier: GPL-3.0-or-later
# convert/: tags, transcoding, artwork, verification, the quality advisor.
# Contract: docs/contracts/media.md; design: DESIGN.md next to this file.

find_package(taglib CONFIG REQUIRED)  # 2.3.2 from the overlay port; the build checks the version (detail/TagLibUtil.h)
find_package(mp3lame CONFIG REQUIRED) # MP3 is encoded by libmp3lame directly (genuine LAME tag, conversion §2.1)

qt_add_library(selecta_convert STATIC
    MetadataModel.h MetadataModel.cpp
    TagReader.h TagReader.cpp
    TagPolicy.h TagPolicy.cpp
    TagWriter.h TagWriter.cpp
    EncodeSettings.h EncodeSettings.cpp
    OutputVerifier.h OutputVerifier.cpp
    Transcoder.h Transcoder.cpp
    ArtworkProcessor.h ArtworkProcessor.cpp
    ArtworkFinder.h ArtworkFinder.cpp
    detail/Encoder.h
    detail/FfmpegEncoder.cpp
    detail/LameEncoder.cpp
    detail/WavEncoder.cpp
    detail/OutputFile.h detail/OutputFile.cpp
    detail/PcmHash.h detail/PcmHash.cpp
    detail/Quantizer.h detail/Quantizer.cpp
    detail/TagLibUtil.h detail/TagLibUtil.cpp
    detail/TranscodeSeam.h)
add_library(Selecta::Convert ALIAS selecta_convert)
# Qt Gui for QImage/QImageReader (artwork); never Widgets. FFmpeg comes through selecta_audio (its detail/ glue).
target_link_libraries(selecta_convert PUBLIC selecta_audio Qt6::Gui PRIVATE TagLib::tag mp3lame::mp3lame)
selecta_set_warnings(selecta_convert)
