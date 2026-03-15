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
  PUBLIC
    include/ImageResliceHelper.h
    include/vtkImageOrthoPlanes.h
    include/vtkImageSlicePointPlacer.h
    include/vtkLandmarkActor.h
    include/vtkSliceOutlineSource.h
    include/vtkTransformRepresentation.h
    include/vtkTransformWidget.h
)

target_include_directories(CTAXCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(CTAXCore
  PUBLIC
    ${VTK_LIBRARIES}
    ${ITK_LIBRARIES}
    VTK::DICOM
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