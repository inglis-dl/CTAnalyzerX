#pragma once

#include <array>
#include <string>
#include <vector>
#include <bitset>

#include <itkImage.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

class vtkPolyData;

// Type aliases
using ImageType = itk::Image<unsigned char, 3>;
using FloatImageType = itk::Image<float, 3>;

struct BoneMetrics
{
    double VoxelBVTV = 0.0;
    double StructureModelIndex = 0.0;
    double VoxelTrabecularNumber = 0;
    double VoxelTrabecularThickness = 0;
    double VoxelTrabecularSpacing = 0;
    double VoxelBSBV = 0;
    double MeshVolume = 0; // vtk surface based
    double MeshSurfaceArea = 0; // vtk surface based
    double MeshBSBV = 0; // vtk surface based
    double MeshEulerNumber = 0; // vtk surface based
    double MeshSphericity = 0; // vtk surface based
    double VoxelVolume = 0;
};

struct VoidMetrics
{
    double VoxelVolume = 0.0;
    // elongation of the void computed as the ratio of the largest principal moment by
    // the smallest principal moment. Its value is greater or equal to 1
    double VoxelElongation = 0;
    double VoxelFlatness = 0;
    double VoxelRoundness = 0;

    double MeshSurfaceArea = 0.0;
    // equivalent diameter of the hypersphere of the same size of the label object.
    double VoxelEquivalentDiameter = 0.0;
    // vtk normalized shape index characterizes the deviation of the shape of an object from a sphere. A sphere's NSI is one. This number is always >= 1.0
    double MeshSphericity = 0.0;
};

struct TopologyMetrics
{
    int EulerNumber = 0;
    int Genus = 0;
    int NumComponents = 0;
    int NumCavities = 0;
};

namespace BoneMorphometry
{
    bool ComputeVoidMetrics(ImageType::Pointer itkImage, std::vector<VoidMetrics>& out,
        double& totalVolume, double& totalSurfaceArea);

   bool ComputeBoneMetrics(ImageType::Pointer itkImage, BoneMetrics& out);
    
    double ComputeSMI(ImageType::Pointer itkImage);

    std::vector<VoidMetrics> ExtractAndMeasureVoids(ImageType::Pointer itkImage,
        double& totalVolume, double& totalSurfaceArea);

    // Topology analysis
    int ComputeEulerNumber(ImageType::Pointer binaryImage);
    TopologyMetrics ComputeTopology(ImageType::Pointer img);
    int GetEulerLUT(int config); // Helper for manual computation

    // Utilities
    ImageType::Pointer LoadImage(const std::string& filename);
    
    vtkSmartPointer<vtkImageData> ITKToVTKImage(ImageType::Pointer itkImage);
    
    ImageType::Pointer VTKToITKImage(vtkImageData* vtkImage);

    FloatImageType::Pointer VTKToITKFloatImage(vtkImageData* vtkImage);

    vtkSmartPointer<vtkPolyData> ExtractSurface(vtkImageData* img, double threshold = 0);

    int ComputeEulerNumberFromSurface(vtkPolyData* surface);

    bool WriteVoidsLabeledImage(ImageType::Pointer itkImage,
                         const std::string& outputFilename, bool uniqueLabels = false);

    bool WriteSurfaces(ImageType::Pointer itkImage,
        const std::string& outputBoneFilename,
        const std::string& outputVoidsFilename);

} // namespace BoneMorphometry