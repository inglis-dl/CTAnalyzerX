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

# ITK is found unconditionally in the root CMakeLists.txt (all components).
# include(ITK_USE_FILE) activates the compiler flags and include-path variables
# that ITK headers require (e.g. itkConfigure.h include directory).
include(${ITK_USE_FILE})

# ITK bundles itkdouble-conversion-5.2.lib which duplicates the double-conversion
# symbols already present in Qt5Core[d].lib, causing LNK2005/LNK1169 on MSVC.
# Mirror the same filter applied in CTAXCore: strip the ITK copy from the link
# line so only Qt's copy is linked into the final binary.
set(CTAXPROTOTYPE_ITK_LIBS ${ITK_LIBRARIES})
list(FILTER CTAXPROTOTYPE_ITK_LIBS EXCLUDE REGEX "itkdouble-conversion")

# ---------------------------------------------------------------------------
# ImageGraphCut3DSegmentation  (graph-cut bone island segmentation)
#
# This is a header-only INTERFACE library built from the ITKGraphCutSegmentation
# repository and installed via cmake --install.
#
# Resolved via CMAKE_PREFIX_PATH, which must point at the install prefix used
# when building ITKGraphCutSegmentation, e.g.:
#   cmake -DCMAKE_PREFIX_PATH=D:/build/install ...
#
# find_package locates the package config at:
#   <prefix>/lib/cmake/ImageGraphCut3DSegmentation/
#       ImageGraphCut3DSegmentationConfig.cmake
#       ImageGraphCut3DSegmentationTargets.cmake
#       ImageGraphCut3DSegmentationConfigVersion.cmake
#
# The imported INTERFACE target ImageGraphCut3DSegmentation::ImageGraphCut3DSegmentation
# propagates two include roots to all consumers:
#
#   1. <prefix>/include/ImageGraphCut3DSegmentation/
#      Provides:
#        GraphCut.h                  - backend selector (#include "gridcut_config.h")
#        GraphCutImageFilter.h       - base filter interface
#        ImageGridCutFilter.h        - GridCut multi-threaded backend
#        ImageGraphCut3DFilter.h     - shared 3-D filter base
#        ImageGraphCut3DKolmogorovFilter.hxx - Kolmogorov single-threaded backend
#        gridcut_config.h            - generated; defines GRIDCUT_LIBRARY_AVAILABLE
#        lib/kolmogorov-3.03/        - Kolmogorov maxflow sources (header + cpp)
#        lib/gridcut/include/GridCut/- GridCut headers (see root 2)
#
#   2. <prefix>/include/ImageGraphCut3DSegmentation/lib/gridcut/include/
#      Provides the short-form GridCut include path:
#        GridCut/GridGraph_3D_6C_MT.h - multi-threaded 3-D 6-connected graph
#        GridCut/GridGraph_3D_6C.h
#        GridCut/GridGraph_2D_4C.h
#        ...
#
# Backend selection - controlled by gridcut_config.h:
#
#   GRIDCUT_LIBRARY_AVAILABLE defined (GridCut found at ITKGraphCutSegmentation
#   configure time via -DGRIDCUT_SOURCE_DIR=<path>):
#     GraphCut.h selects itk::ImageGridCutFilter  - multi-threaded GridCut solver.
#     GridGraph_3D_6C_MT.h is referenced via a one-line forwarding header
#     generated at configure time; the real header is read directly from
#     GRIDCUT_SOURCE_DIR - no copy is made in the build tree.
#
#   GRIDCUT_LIBRARY_AVAILABLE not defined (GRIDCUT_SOURCE_DIR not set or not found):
#     GraphCut.h selects itk::ImageGraphCut3DKolmogorovFilter - single-threaded
#     Kolmogorov maxflow solver bundled with ITKGraphCutSegmentation.
#
# Usage in PrototypeHelpers.cpp:
#   #include "GraphCut.h"
#   using GCFilter = GraphCut::FilterType<InputImage, BinaryImage,
#                                         BinaryImage, BinaryImage>;
# ---------------------------------------------------------------------------
find_package(ImageGraphCut3DSegmentation REQUIRED)

# Link against the GUI lib that provides VolumeView/ImageFrameWidget/JsonUtils and against core.
target_link_libraries(CTAXPrototype
  PRIVATE
    CTAXGui
    ${CTAXPROTOTYPE_VTK_LIBS}
    ${CTAXPROTOTYPE_ITK_LIBS}
    # Header-only INTERFACE target - propagates both include roots and the
    # GRIDCUT_LIBRARY_AVAILABLE compile definition when GridCut was found.
    ImageGraphCut3DSegmentation::ImageGraphCut3DSegmentation
    Qt5::Concurrent
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