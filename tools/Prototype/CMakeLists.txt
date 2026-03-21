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

# VTK modules used directly by PrototypeHelpers (morphological pipeline).
# ImagingGeneral       : vtkImageGaussianSmooth
# ImagingMorphological : vtkImageContinuousErode3D / vtkImageContinuousDilate3D
# ImagingStatistics    : vtkImageThresholdConnectivity
set(CTAXPROTOTYPE_VTK_LIBS
  VTK::ImagingGeneral
  VTK::ImagingMorphological
  VTK::ImagingStatistics
)

# ITK is already found unconditionally in the root CMakeLists.txt (all components).
# include(ITK_USE_FILE) activates the compiler flags and include-path variables
# that ITK headers require (e.g. itkConfigure.h include directory).
include(${ITK_USE_FILE})

# ITK bundles itkdouble-conversion-5.2.lib which duplicates the double-conversion
# symbols already present in Qt5Core[d].lib, causing LNK2005/LNK1169 on MSVC.
# Mirror the same filter applied in CTAXCore: strip the ITK copy from the link
# line so only Qt's copy is linked into the final binary.
set(CTAXPROTOTYPE_ITK_LIBS ${ITK_LIBRARIES})
list(FILTER CTAXPROTOTYPE_ITK_LIBS EXCLUDE REGEX "itkdouble-conversion")

# Link against the GUI lib that provides VolumeView/ImageFrameWidget/JsonUtils and against core.
target_link_libraries(CTAXPrototype
  PRIVATE
    CTAXGui
    ${CTAXPROTOTYPE_VTK_LIBS}
    ${CTAXPROTOTYPE_ITK_LIBS}
)

# Register the new VTK modules with the autoinit system so VTK's
# object-factory overrides are correctly wired at runtime.
vtk_module_autoinit(
  TARGETS CTAXPrototype
  MODULES ${CTAXPROTOTYPE_VTK_LIBS}
)

# Ensure uic sees Prototype's .ui files AND any ui_*.h included from linked libs (e.g., CTAXGui)
set_property(TARGET CTAXPrototype PROPERTY AUTOUIC_SEARCH_PATHS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources
  ${CTAX_GUI_RESOURCES_DIR}
)

# VTK autoinit is handled by CTAXCore and CTAXGui, no need to repeat here