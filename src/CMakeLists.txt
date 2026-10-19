# SPDX-License-Identifier: GPL-3.0-or-later
# Module layout and dependency rules: docs/PLAN.md §2.2 and references/qt-app-architecture.md §12.
# Nothing below app/ may use Qt Widgets.

add_subdirectory(base)
add_subdirectory(governor)
add_subdirectory(storage)
add_subdirectory(app)
