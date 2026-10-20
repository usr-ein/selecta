# SPDX-License-Identifier: GPL-3.0-or-later
# audio/: decoding (growing files included), 3-band waveform analysis and its cache, quality checks and spectrograms.
# Contract: docs/contracts/media.md; design: DESIGN.md next to this file.

find_package(FFMPEG REQUIRED)            # avcodec, avformat, swresample, avutil (static, vcpkg)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SELECTA_SOXR REQUIRED IMPORTED_TARGET soxr)
find_package(ZLIB REQUIRED)

qt_add_library(selecta_audio STATIC
    Error.h Error.cpp
    ByteSource.h ByteSource.cpp
    StreamInfo.h StreamInfo.cpp
    Decoder.h Decoder.cpp
    Waveform.h Waveform.cpp
    BandAnalyzer.h BandAnalyzer.cpp
    WaveformAnalyzer.h WaveformAnalyzer.cpp
    AnalysisCache.h AnalysisCache.cpp
    Spectrogram.h Spectrogram.cpp
    QualityCheck.h QualityCheck.cpp
    detail/ThreadHandOver.h
    detail/AvioAdapter.h detail/AvioAdapter.cpp
    detail/Backend.h
    detail/Bytes.h
    detail/ContainerProbe.h detail/ContainerProbe.cpp
    detail/Crc.h detail/Crc.cpp
    detail/Ffmpeg.h detail/Ffmpeg.cpp
    detail/FfmpegBackend.h detail/FfmpegBackend.cpp
    detail/FlacFormat.h detail/FlacFormat.cpp
    detail/IndexedBackend.h detail/IndexedBackend.cpp
    detail/MpegAudio.h detail/MpegAudio.cpp
    detail/OutputStage.h detail/OutputStage.cpp
    detail/ParseStatus.h
    detail/PcmFormat.h detail/PcmFormat.cpp
    detail/SourceReader.h detail/SourceReader.cpp
    detail/SpectrumAnalysis.h detail/SpectrumAnalysis.cpp)
add_library(Selecta::Audio ALIAS selecta_audio)
target_link_libraries(selecta_audio PUBLIC selecta_base Qt6::Core)
# FFmpeg's headers are third-party code: SYSTEM keeps their C casts out of our warnings. PUBLIC because convert/ and the
# tests use the FFmpeg glue in detail/.
target_include_directories(selecta_audio SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_directories(selecta_audio PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(selecta_audio PUBLIC ${FFMPEG_LIBRARIES} PkgConfig::SELECTA_SOXR ZLIB::ZLIB)
selecta_set_warnings(selecta_audio)
