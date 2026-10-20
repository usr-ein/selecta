# SPDX-License-Identifier: GPL-3.0-or-later
# share/: the share index and the upload queue (docs/contracts/share.md). Walks the shared folders, read-only, into an
# immutable index that answers peers from memory; decides the order of uploads. No database, no settings, no network.

qt_add_library(selecta_share STATIC
    ShareIndex.h ShareIndex.cpp
    Open.h Open.cpp
    UploadQueue.h UploadQueue.cpp
    detail/Attributes.h detail/Attributes.cpp
    detail/Descriptor.h detail/Descriptor.cpp
    detail/IndexBuilder.h detail/IndexBuilder.cpp
    detail/IndexData.h detail/IndexData.cpp
    detail/Query.h detail/Query.cpp
    detail/Text.h detail/Text.cpp
    detail/Walk.h detail/Walk.cpp)
add_library(Selecta::Share ALIAS selecta_share)
target_include_directories(selecta_share PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(selecta_share PUBLIC selecta_slsk selecta_collection selecta_audio selecta_match Qt6::Core)
selecta_set_warnings(selecta_share)
