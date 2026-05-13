add_executable(CTAXBatchProcessor WIN32)

target_sources(CTAXBatchProcessor
  PRIVATE
    src/main.cpp
    src/BatchProcessor.cpp
    src/BatchProgressDialog.cpp
  PUBLIC
    include/BatchProcessorTypes.h
    include/BatchProcessor.h
    include/BatchProgressDialog.h
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


set_property(TARGET CTAXBatchProcessor PROPERTY AUTOUIC_SEARCH_PATHS
  ${CTAX_GUI_RESOURCES_DIR}
)