
#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkImageViewer2.h>
#include <vtkSmartPointer.h>

// Add member variables to MainWindow class
vtkSmartPointer<vtkImageViewer2> viewer;
QSlider *sliceSlider = nullptr;
QLabel *sliceLabel = nullptr;

void MainWindow::setupSliceControls()
{
    // Create slider and label
    sliceSlider = new QSlider(Qt::Horizontal);
    sliceLabel = new QLabel("Slice: 0");

    sliceSlider->setMinimum(0);
    sliceSlider->setMaximum(0);
    sliceSlider->setValue(0);

    // Connect slider to slice update
    connect(sliceSlider, &QSlider::valueChanged, this, [=](int value) {
        if (viewer) {
            viewer->SetSlice(value);
            viewer->Render();
            sliceLabel->setText(QString("Slice: %1").arg(value));
        }
    });

    // Add to dock widget layout
    QWidget *dockWidget = ui->dockSliceControls->widget();
    QVBoxLayout *layout = new QVBoxLayout(dockWidget);
    layout->addWidget(sliceLabel);
    layout->addWidget(sliceSlider);
    dockWidget->setLayout(layout);
}

void MainWindow::loadVolume(vtkSmartPointer<vtkImageData> imageData)
{
    viewer = vtkSmartPointer<vtkImageViewer2>::New();
    viewer->SetInputData(imageData);
    viewer->SetupInteractor(ui->centralwidget->findChild<QVTKOpenGLNativeWidget *>()->GetInteractor());
    viewer->SetRenderWindow(ui->centralwidget->findChild<QVTKOpenGLNativeWidget *>()->GetRenderWindow());
    viewer->SetSliceOrientationToXY();
    viewer->SetSlice(0);
    viewer->Render();

    // Update slider range
    int maxSlice = viewer->GetSliceMax();
    sliceSlider->setMaximum(maxSlice);
    sliceSlider->setValue(0);
    sliceLabel->setText("Slice: 0");
}
