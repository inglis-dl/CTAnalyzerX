
#ifndef VIEWFACTORY_H
#define VIEWFACTORY_H

/*
#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include "SliceView.h"
#include "VolumeView.h"

class ViewFactory {
public:
	static QVTKOpenGLNativeWidget* createVolumeView(vtkSmartPointer<vtkImageData> imageData);
	static QVTKOpenGLNativeWidget* createSliceView(vtkSmartPointer<vtkImageData> imageData, SliceOrientation orientation);
};

#endif // VIEWFACTORY_H


#pragma once
*/

#include "SliceView.h"
#include "VolumeView.h"

class ViewFactory {
public:
	static SliceView* createSliceView(SliceView::Orientation orientation, QWidget* parent = nullptr);
	static VolumeView* createVolumeView(QWidget* parent = nullptr);
};

#endif // VIEWFACTORY_H
