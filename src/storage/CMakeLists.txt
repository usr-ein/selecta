# SPDX-License-Identifier: GPL-3.0-or-later
qt_add_library(selecta_storage STATIC
    Database.h Database.cpp
    Migrations.h Migrations.cpp
    SessionLog.h SessionLog.cpp)
add_library(Selecta::Storage ALIAS selecta_storage)
target_link_libraries(selecta_storage PUBLIC selecta_base PRIVATE Selecta::SQLite3)
selecta_set_warnings(selecta_storage)
