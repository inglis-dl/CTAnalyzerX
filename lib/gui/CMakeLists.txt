add_library(CTAXGui STATIC)

target_sources(CTAXGui
  PRIVATE
    src/CollapsibleGroupBox.cpp
    src/CropExporter.cpp
    src/CropWidget.cpp
    src/ImageFrameWidget.cpp
    src/ImageInfoWidget.cpp
    src/ImageLoader.cpp
    src/JsonSettings.cpp
    src/JsonUtils.cpp
    src/LandmarkHelper.cpp
    src/LandmarkWidget.cpp
    src/LightboxWidget.cpp
    src/Logger.cpp
    src/MenuButton.cpp
    src/OtsuThresholdWorker.cpp
    src/ProxyStyle.cpp
    src/RangeSlider.cpp
    src/ScalarOpacityFunctionWidget.cpp
    src/SelectionFrameWidget.cpp
    src/SliceView.cpp
    src/SunkenSliderStyle.cpp
    src/VolumeRotationWidget.cpp
    src/VolumeView.cpp
    src/WindowLevelBridge.cpp
#    src/WindowLevelBus.cpp
    src/WindowLevelWidget.cpp
    resources/CropWidget.ui
    resources/ImageInfoWidget.ui
    resources/LandmarkWidget.ui
    resources/LightboxWidget.ui
    resources/ScalarOpacityFunctionWidget.ui
    resources/SliceView.ui
    resources/VolumeRotationWidget.ui
    resources/VolumeView.ui
    resources/WindowLevelWidget.ui
    resources/resources.qrc
  PUBLIC
    include/CollapsibleGroupBox.h
    include/CropExporter.h
    include/CropWidget.h
    include/ImageFrameWidget.h
    include/ImageInfoWidget.h
    include/ImageLoader.h
    include/JsonSettings.h
    include/JsonUtils.h
    include/LandmarkHelper.h
    include/LandmarkWidget.h
    include/LightboxWidget.h
    include/Logger.h
    include/MenuButton.h
    include/OtsuThresholdWorker.h
    include/ProxyStyle.h
    include/RangeSlider.h
    include/ScalarOpacityFunctionWidget.h
    include/SelectionFrameWidget.h
    include/SliceView.h
    include/SunkenSliderStyle.h
    include/VolumeRotationWidget.h
    include/VolumeView.h
    include/WindowLevelBridge.h
#    include/WindowLevelBus.h
    include/WindowLevelWidget.h
)

target_include_directories(CTAXGui
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(CTAXGui
  PUBLIC
    CTAXCore
    Qt${VTK_QT_VERSION}::Core
    Qt${VTK_QT_VERSION}::Gui
    Qt${VTK_QT_VERSION}::Widgets
    Qt${VTK_QT_VERSION}::Charts
)

# Ensure uic can find .ui templates that live under lib/gui/resources
set_property(TARGET CTAXGui PROPERTY AUTOUIC_SEARCH_PATHS
  ${CMAKE_CURRENT_SOURCE_DIR}/resources
)