add_executable(CTAXMorphometry)

target_sources(CTAXMorphometry
  PRIVATE
    src/main.cpp
    src/BoneMorphometry.cpp
  PUBLIC
    include/BoneMorphometry.h
)

target_include_directories(CTAXMorphometry
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(CTAXMorphometry PRIVATE cxx_std_17)

find_package(ITK REQUIRED)
include(${ITK_USE_FILE})

set(CTAXMORPHOMETRY_VTK_LIBS
  VTK::ImagingGeneral
  VTK::ImagingMorphological
  VTK::ImagingStatistics
  VTK::FiltersCore
  VTK::FiltersGeneral
)

target_link_libraries(CTAXMorphometry
  PRIVATE
    ${CTAXMORPHOMETRY_VTK_LIBS}
    ${ITK_LIBRARIES}
)

vtk_module_autoinit(
  TARGETS CTAXMorphometry
  MODULES ${CTAXMORPHOMETRY_VTK_LIBS}
)