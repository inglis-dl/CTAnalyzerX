add_executable(CTAXPrototype WIN32)

target_sources(CTAXPrototype
  PRIVATE
    src/main.cpp
    src/PrototypeMainWindow.cpp
    src/PrototypeHelpers.cpp
    resources/MainWindow.ui
  PUBLIC
    include/PrototypeMainWindow.h
    include/PrototypeHelpers.h
)

# Make sure generated ui_*.h is found and local headers are visible.
target_include_directories(CTAXPrototype
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against the GUI lib that provides VolumeView/ImageFrameWidget/JsonUtils and against core.
target_link_libraries(CTAXPrototype
  PRIVATE
    CTAXGui
)

# Ensure uic sees Prototype's .ui files AND any ui_*.h included from linked libs (e.g., CTAXGui)
set_property(TARGET CTAXPrototype PROPERTY AUTOUIC_SEARCH_PATHS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources
  ${CTAX_GUI_RESOURCES_DIR}
)

# VTK autoinit is handled by CTAXCore and CTAXGui, no need to repeat here