add_executable(CTAXBatchProcessor WIN32)

target_sources(CTAXBatchProcessor
  PRIVATE
    src/main.cpp
    src/BatchProcessor.cpp
  PUBLIC
    include/BatchProcessor.h
)

# Make sure generated ui_*.h is found and local headers are visible.
target_include_directories(CTAXBatchProcessor
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# VTK modules used directly by PrototypeHelpers (morphological pipeline).
# ImagingGeneral       : vtkImageGaussianSmooth
# ImagingMorphological : vtkImageContinuousErode3D / vtkImageContinuousDilate3D
# ImagingStatistics    : vtkImageThresholdConnectivity
set(CTAXBATCH_VTK_LIBS
  VTK::ImagingGeneral
  VTK::ImagingMorphological
  VTK::ImagingStatistics
)

# Link against the GUI lib that provides VolumeView/ImageFrameWidget/JsonUtils and against core.
target_link_libraries(CTAXBatchProcessor
  PRIVATE
    CTAXGui
    ${CTAXBATCH_VTK_LIBS}
    Qt5::Concurrent
)

# Register the new VTK modules with the autoinit system so VTK's
# object-factory overrides are correctly wired at runtime.
#vtk_module_autoinit(
#  TARGETS CTAXBatchProcessor
#  MODULES ${CTAXBATCH_VTK_LIBS}
#)

set_property(TARGET CTAXBatchProcessor PROPERTY AUTOUIC_SEARCH_PATHS
#  ${CMAKE_CURRENT_SOURCE_DIR}/resources
  ${CTAX_GUI_RESOURCES_DIR}
)