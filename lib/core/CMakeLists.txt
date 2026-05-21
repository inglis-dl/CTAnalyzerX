add_library(CTAXCore STATIC)

target_sources(CTAXCore
  PRIVATE
    src/ImageResliceHelper.cpp
    src/vtkImageOrthoPlanes.cpp
    src/vtkImageSlicePointPlacer.cpp
    src/vtkLandmarkActor.cpp
    src/vtkSliceOutlineSource.cpp
    src/vtkTransformRepresentation.cxx
    src/vtkTransformWidget.cxx
	src/vtkImageCoordinateWidget.cxx
    src/ProcessHelpers.cpp
    src/VtkQtOutputWindow.cpp
    src/Logger.cpp
    src/OtsuThresholdWorker.cpp
    src/VoxelLineIterator.cpp
  PUBLIC
    include/ImageResliceHelper.h
    include/vtkImageOrthoPlanes.h
    include/vtkImageSlicePointPlacer.h
    include/vtkLandmarkActor.h
    include/vtkSliceOutlineSource.h
    include/vtkTransformRepresentation.h
    include/vtkTransformWidget.h
	include/vtkImageCoordinateWidget.h
    include/ProcessHelpers.h
    include/VtkQtOutputWindow.h
    include/Logger.h
    include/OtsuThresholdWorker.h
    include/VoxelLineIterator.h
)

target_include_directories(CTAXCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Ensure targets linking CTAXCore can compile code that includes ITK headers (e.g., itkVersion.h)
if(ITK_INCLUDE_DIRS)
  target_include_directories(CTAXCore PUBLIC ${ITK_INCLUDE_DIRS})
endif()

set(CTAXCORE_VTK_LIBS
  VTK::ImagingCore
  VTK::ImagingGeneral
  VTK::RenderingCore
  VTK::RenderingOpenGL2
  VTK::InteractionStyle
  VTK::RenderingContextOpenGL2
  VTK::RenderingVolume
  VTK::RenderingVolumeOpenGL2
  VTK::DICOM
)

# ITK brings in itkdouble-conversion-5.2.lib, which conflicts with Qt5Core[d].lib's
# internal double-conversion.obj on Windows. Remove it from the link line.
set(CTAXCORE_ITK_LIBS ${ITK_LIBRARIES})
list(FILTER CTAXCORE_ITK_LIBS EXCLUDE REGEX "itkdouble-conversion")

message(STATUS "CTAXCore: linking ITK libs (filtered): ${CTAXCORE_ITK_LIBS}")

target_link_libraries(CTAXCore
  PUBLIC
    Qt${VTK_QT_VERSION}::Core
    ${CTAXCORE_VTK_LIBS}
  PRIVATE
    ${CTAXCORE_ITK_LIBS}
)

vtk_module_autoinit(
  TARGETS CTAXCore
  MODULES ${CTAXCORE_VTK_LIBS}
)

target_compile_definitions(CTAXCore
  PUBLIC
    CTANALYZERX_VERSION="${PROJECT_VERSION}"
    CTANALYZERX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    CTANALYZERX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    CTANALYZERX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    CTANALYZERX_BUILD_DATE="${CTANALYZERX_BUILD_DATE}"
    CTANALYZERX_GIT_HASH="${CTANALYZERX_GIT_HASH}"
    CTANALYZERX_BUILD_TYPE="$<CONFIG>"
    CTANALYZERX_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    CTANALYZERX_SYSTEM="${CMAKE_SYSTEM_NAME}"
    CTANALYZERX_ARCH="${CMAKE_SYSTEM_PROCESSOR}"
    CTANALYZERX_VTKDICOM_VERSION="${CTANALYZERX_VTKDICOM_VERSION}"
)