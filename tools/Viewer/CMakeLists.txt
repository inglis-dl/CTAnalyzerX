add_executable(CTAXViewer WIN32)

target_sources(CTAXViewer
  PRIVATE
    src/main.cpp
)

target_link_libraries(CTAXViewer
  PRIVATE
    CTAXGui
)

# If you add .ui / .qrc under tools/Viewer/resources later, you can also set:
# set_property(TARGET CTAXViewer PROPERTY AUTOUIC_SEARCH_PATHS
#   ${CMAKE_CURRENT_SOURCE_DIR}/resources
# )