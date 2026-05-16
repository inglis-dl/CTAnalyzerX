#pragma once

#include <array>
#include <string>
#include <vector>

#include <vtkSmartPointer.h>
#include <vtkImageData.h>

//class vtkImageData;
class vtkPolyData;

struct BoneMetrics
{
    double bvtv = 0.0;// Bone volume fraction
    double smi = 0.0;// Structure model index
    double meanTrabecularThickness = 0.0;// From MedialThicknessImageFilter3D
    double maxTrabecularThickness;
    double meanCorticalThickness = 0.0;// From VTK surface-based MAT
};

struct VoidMetrics
{
    double volume = 0.0;
    double surfaceArea = 0.0;
    double equivalentDiameter = 0.0;
    double sphericity = 0.0;
    double thickness = 0.0;// From MedialThicknessImageFilter3D
    int eulerCharacteristic = 0;// From LabelGeometryImageFilter
    std::array<double, 3> centroid{ { 0.0, 0.0, 0.0 } };
};

struct MATMetrics
{
    double meanRadius = 0.0;
    double maxRadius = 0.0;
};

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
namespace BoneMorphometry
{
    // High-level solid bone morphometry
    bool ComputeBoneMetrics(const std::string& filename, BoneMetrics& out);

    // High-level void morphometry
    bool ComputeVoidMetrics(const std::string& filename, std::vector<VoidMetrics>& out);

    // --------------------------------------------------------
    // Solid bone components
    // --------------------------------------------------------
    double ComputeBVTV(const std::string& filename);
    double ComputeSMI(const std::string& filename);

    // Voxel-based MAT (ITK MedialThicknessImageFilter3D)
    MATMetrics ComputeVoxelMAT(const std::string& filename);

    // Surface-based cortical thickness (VTK implicit distance)
    double ComputeSurfaceThickness(vtkPolyData* surface);

    // --------------------------------------------------------
    // Void analysis
    // --------------------------------------------------------
    std::vector<VoidMetrics> ExtractAndMeasureVoids(const std::string& filename);

    // --------------------------------------------------------
    // Utility functions
    // --------------------------------------------------------
    vtkSmartPointer<vtkImageData> ITKToVTKImage(const std::string& filename);
    vtkPolyData* ExtractSurface(vtkImageData* img);

    int ComputeEulerCharacteristicFromSurface(vtkPolyData* surface);

    // Euler characteristic via LabelGeometryImageFilter
    int ComputeEulerCharacteristicITK(const std::string& filename,
    unsigned int labelValue);

    // Thickness via MedialThicknessImageFilter3D
    double ComputeMedialThicknessITK(const std::string& filename,
    unsigned int labelValue);

} // namespace BoneMorphometry