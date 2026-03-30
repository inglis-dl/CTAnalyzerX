add_executable(CTAXViewer WIN32)

target_sources(CTAXViewer
  PRIVATE
    src/main.cpp
    src/ViewerMainWindow.cpp
    include/ViewerMainWindow.h
)

target_include_directories(CTAXViewer
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(CTAXViewer
  PRIVATE
    CTAXGui
)

set_property(TARGET CTAXViewer PROPERTY AUTOUIC ON)
set_property(TARGET CTAXViewer PROPERTY AUTOMOC ON)
set_property(TARGET CTAXViewer PROPERTY AUTORCC ON)

# AUTOUIC must be able to find .ui files that belong to CTAXGui headers
# (e.g. CropWidget.ui, WindowLevelWidget.ui, etc.) when they are transitively
# included by ViewerMainWindow headers. Point it at the same resources directory
# that CTAXGui itself declares.
set_property(TARGET CTAXViewer PROPERTY AUTOUIC_SEARCH_PATHS
  ${CMAKE_SOURCE_DIR}/lib/gui/resources
)