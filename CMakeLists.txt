cmake_minimum_required(VERSION 3.16...3.21 FATAL_ERROR)

project(CTAnalyzerX
    VERSION 0.1.0
    DESCRIPTION "3D volume image visualization tool for DICOM and Scanco .isq files"
    LANGUAGES CXX
)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enforce out-of-source builds
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR "CTAnalyzerX requires an out-of-source build. Please use a separate build directory.")
endif()

# Custom CMake modules
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Build type defaults
set(DEFAULT_BUILD_TYPE "Debug")
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported build types" FORCE)
    set_property(CACHE CMAKE_CONFIGURATION_TYPES PROPERTY STRINGS "Debug" "Release")
elseif(NOT CMAKE_BUILD_TYPE)
    message(STATUS "Setting build type to '${DEFAULT_BUILD_TYPE}'")
    set(CMAKE_BUILD_TYPE "${DEFAULT_BUILD_TYPE}" CACHE STRING "Choose the type of build." FORCE)
endif()

# Globally silence MSVC deprecation warnings for non-standard stdext iterators
if(MSVC)
    add_compile_definitions($<$<CXX_COMPILER_ID:MSVC>:_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING>)
    add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
    # Treat all source files as UTF-8; eliminates the code-page prompt and
    # ensures Unicode literals in comments/strings compile without warnings.
    add_compile_options(/utf-8)
endif()

# Work around ITK/Qt double-conversion symbol collisions on MSVC.
# Qt5Core[d].lib contains double-conversion objects; ITK may also link itkdouble-conversion-5.2.lib.
# If both are linked into the same final binary, LNK2005 occurs.
function(ctax_msvc_exclude_itk_double_conversion target_name)
  if(MSVC)
    target_link_options(
        ${target_name} PRIVATE 
        "/NODEFAULTLIB:itkdouble-conversion-5.2.lib"
        "/NODEFAULTLIB:itkdouble-conversion.lib"
    )
  endif()
endfunction()

# Output directories
include(GNUInstallDirs)
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# MSVC runtime settings
if(MSVC)
    if(BUILD_SHARED_LIBS)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# ========================
# Find VTK FIRST
# ========================
find_package(VTK 9.3 REQUIRED COMPONENTS
  ImagingCore
  ImagingGeneral
  RenderingCore
  RenderingOpenGL2
  InteractionStyle
  RenderingContextOpenGL2
  RenderingVolume
  RenderingVolumeOpenGL2
  GUISupportQt
  QUIET
)

if(NOT VTK_FOUND)
  message(FATAL_ERROR "Unable to find the VTK build folder.")
endif()

# ========================
# Determine Qt version from VTK
# ========================
if(NOT DEFINED VTK_QT_VERSION)
  set(VTK_QT_VERSION 5)
endif()

# ========================
# Find Qt AFTER VTK
# ========================
set(qt_components Core Gui Widgets Charts Concurrent)

find_package(Qt${VTK_QT_VERSION}
    CONFIG
    REQUIRED
    COMPONENTS ${qt_components}
    QUIET
)

message(STATUS "Found Qt${VTK_QT_VERSION} with components: ${qt_components}")

# ========================
# Find ITK
# ========================
find_package(ITK 5.2.0 CONFIG REQUIRED QUIET)

if(NOT ITK_FOUND)
  message(FATAL_ERROR "Unable to find the ITK build folder.")
endif()

# Suppress the ITK/Qt double-conversion symbol collision (LNK2005/LNK1169) on MSVC.
#
# Qt5Core[d].lib embeds double-conversion; ITK also bundles itkdouble-conversion-5.2.lib.
# When both reach the same final executable the linker refuses with duplicate symbols.
#
# The itkdouble-conversion IMPORTED target carries its library path in
# INTERFACE_LINK_LIBRARIES on every ITK target that depends on it, as well as
# directly in ITK_LIBRARIES.  The approach that works for ALL propagation paths
# is to strip "itkdouble-conversion" from INTERFACE_LINK_LIBRARIES on every ITK
# target that lists it, AND filter it from ITK_LIBRARIES.
# Setting IMPORTED_LOCATION to "" does NOT work because CMake stores the sentinel
# "itkdouble-conversion-NOTFOUND" and then emits that string literally to the linker.
if(MSVC)
  # Collect every IMPORTED target defined so far and strip itkdouble-conversion
  # from its INTERFACE_LINK_LIBRARIES.  This covers ITKCommon, ITKVtkGlue, ITKVTK,
  # and any other ITK target regardless of name without maintaining a hard-coded list.
  get_property(_all_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)

  # BUILDSYSTEM_TARGETS only lists targets in the current directory at this point
  # (subdirectories not yet processed).  Use the ITK_LIBRARIES list instead to
  # enumerate every ITK target name reliably — it contains them all as CMake targets.
  foreach(_itk_lib IN LISTS ITK_LIBRARIES)
    if(TARGET "${_itk_lib}")
      get_target_property(_iface "${_itk_lib}" INTERFACE_LINK_LIBRARIES)
      if(_iface)
        list(FILTER _iface EXCLUDE REGEX "itkdouble-conversion")
        set_target_properties("${_itk_lib}" PROPERTIES
          INTERFACE_LINK_LIBRARIES "${_iface}")
      endif()
    endif()
  endforeach()
  unset(_itk_lib)
  unset(_iface)
  unset(_all_targets)

  # Also filter the ITK_LIBRARIES variable itself so any CMakeLists.txt that
  # passes ${ITK_LIBRARIES} directly to target_link_libraries is also clean.
  list(FILTER ITK_LIBRARIES EXCLUDE REGEX "itkdouble-conversion")
  message(STATUS "ITK_LIBRARIES (filtered): ${ITK_LIBRARIES}")
endif()

# ========================
# Find Eigen3 (required by ITK headers when ITK_USE_SYSTEM_EIGEN is ON)
# ========================
find_package(Eigen3 3.3 CONFIG REQUIRED)

find_package(DICOM 0.8.15 CONFIG REQUIRED)

if(NOT DICOM_FOUND)
  message(FATAL_ERROR "Unable to find the vtk-dicom build folder.")
endif()

# Expose VTK-DICOM version (if the package provides it)
set(CTANALYZERX_VTKDICOM_VERSION "unknown")
if(DEFINED DICOM_VERSION)
  set(CTANALYZERX_VTKDICOM_VERSION "${DICOM_VERSION}")
elseif(DEFINED DICOM_VERSION_STRING)
  set(CTANALYZERX_VTKDICOM_VERSION "${DICOM_VERSION_STRING}")
endif()

# ------------------------
# Versioning
# ------------------------
# Store the git hash of the current head
set(CTANALYZERX_GIT_HASH "unknown")
if(EXISTS "${PROJECT_SOURCE_DIR}/.git/HEAD")
  file(READ "${PROJECT_SOURCE_DIR}/.git/HEAD" CTANALYZERX_SOURCE_VERSION)
  if("${CTANALYZERX_SOURCE_VERSION}" MATCHES "^ref:")
    string(REGEX REPLACE "^ref: *([^ \n\r]*).*" "\\1" CTANALYZERX_GIT_REF "${CTANALYZERX_SOURCE_VERSION}")
    if(EXISTS "${PROJECT_SOURCE_DIR}/.git/${CTANALYZERX_GIT_REF}")
      file(READ "${PROJECT_SOURCE_DIR}/.git/${CTANALYZERX_GIT_REF}" CTANALYZERX_SOURCE_VERSION)
      message(STATUS "GIT REF: ${CTANALYZERX_GIT_REF}")
    endif()
  endif()
  string(STRIP "${CTANALYZERX_SOURCE_VERSION}" CTANALYZERX_SOURCE_VERSION)
  if(CTANALYZERX_SOURCE_VERSION)
    set(CTANALYZERX_GIT_HASH "${CTANALYZERX_SOURCE_VERSION}")
  endif()
endif()
message(STATUS "GIT HASH: ${CTANALYZERX_GIT_HASH}")

# Store the build date
set(CTANALYZERX_BUILD_DATE "")
if(WIN32)
  execute_process(COMMAND "cmd" " /c date /t" OUTPUT_VARIABLE CTANALYZERX_DATE OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX REPLACE "[^0-9]*(..).*" "\\1" CTANALYZERX_MONTH "${CTANALYZERX_DATE}")
  set(CTANALYZERX_MONTHS "" "Jan" "Feb" "Mar" "Apr" "May" "Jun" "Jul" "Aug" "Sep" "Oct" "Nov" "Dec")
  list(GET CTANALYZERX_MONTHS "${CTANALYZERX_MONTH}" CTANALYZERX_MONTH)
  string(REGEX REPLACE "[^/]*/(..)/(....).*" "\\1 ${CTANALYZERX_MONTH} \\2" CTANALYZERX_BUILD_DATE "${CTANALYZERX_DATE}")
  execute_process(COMMAND "cmd" " /c echo %TIME%" OUTPUT_VARIABLE CTANALYZERX_TIME OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX REPLACE "[^0-9]*(..:..:..).*" "\\1" CTANALYZERX_BUILD_TIME "${CTANALYZERX_TIME}")
  set(CTANALYZERX_BUILD_DATE "${CTANALYZERX_BUILD_DATE} : ${CTANALYZERX_BUILD_TIME}")
else()
  execute_process(COMMAND "date" "+%d %b %Y/%H:%M:%S" OUTPUT_VARIABLE CTANALYZERX_DATE_TIME OUTPUT_STRIP_TRAILING_WHITESPACE)
  string(REGEX REPLACE "([^/]*)/.*" "\\1" CTANALYZERX_BUILD_DATE "${CTANALYZERX_DATE_TIME}")
  string(REGEX REPLACE "[^/]*/([0-9:]*).*" "\\1" CTANALYZERX_BUILD_TIME "${CTANALYZERX_DATE_TIME}")
  set(CTANALYZERX_BUILD_DATE "${CTANALYZERX_BUILD_DATE} : ${CTANALYZERX_BUILD_TIME}")
endif()
message(STATUS "BUILD DATE: ${CTANALYZERX_BUILD_DATE}")

# Ensure automoc/autorcc/uic for Qt targets in subdirectories
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ------------------------
# Project paths
# ------------------------
set(CTAX_GUI_RESOURCES_DIR "${PROJECT_SOURCE_DIR}/lib/gui/resources")

# ------------------------
# Subdirectories
# ------------------------
add_subdirectory(lib/core)
add_subdirectory(lib/gui)
add_subdirectory(app)

option(CTAX_BUILD_TOOLS "Build tools" ON)
if(CTAX_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

option(CTAX_BUILD_TESTS "Build tests" OFF)
if(CTAX_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# Packaging
set(CPACK_PACKAGE_NAME "CTAnalyzerX")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "3D Volume Image Viewer")
set(CPACK_PACKAGE_VENDOR "Inglis Software Solutions Inc.")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "CTAnalyzerX")
set(CPACK_PACKAGE_EXECUTABLES "CTAnalyzerX" "CTAnalyzerX")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "NSIS")
include(CPack)

# Feature summary
include(FeatureSummary)
feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES PACKAGES_FOUND)
feature_summary(FILENAME ${CMAKE_CURRENT_BINARY_DIR}/features.log WHAT ALL)
