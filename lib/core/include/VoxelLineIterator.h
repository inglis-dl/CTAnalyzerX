#pragma once


/*
================================================================================
VoxelLine / VoxelLineIterator Usage Pattern
================================================================================

This code demonstrates how to use the VoxelLine iterator to traverse a 3D line
through a vtkImageData volume in a safe, efficient, and flexible manner.

--------------------------------------------------------------------------------
OVERVIEW
--------------------------------------------------------------------------------

VoxelLine provides an STL-style interface over a 3D Bresenham voxel traversal.
It allows you to iterate over all voxels intersected by a line segment defined
in world coordinates, while:

  • Automatically converting world coordinates → voxel indices
  • Visiting each voxel exactly once (no duplicates or gaps)
  • Preventing out-of-bounds access
  • Supporting early termination through a user-defined predicate

--------------------------------------------------------------------------------
KEY CONCEPT: EARLY TERMINATION (STOP PREDICATE)
--------------------------------------------------------------------------------

The iterator supports a "stop predicate" — a lambda function that is evaluated
at every visited voxel. When the predicate returns TRUE, iteration stops.

This enables powerful use cases such as:

  • Traversing until leaving a segmented region (e.g., bone)
  • Detecting boundaries or interfaces
  • Limiting traversal to a region of interest
  • Implementing ray-based probing algorithms

--------------------------------------------------------------------------------
HOW IT WORKS
--------------------------------------------------------------------------------

For each voxel along the line:

  1. The iterator computes voxel indices (x,y,z)
  2. The stop predicate is evaluated
  3. If TRUE -> iteration terminates immediately
  4. If FALSE -> voxel is yielded and iteration continues

This avoids unnecessary traversal and improves performance.

--------------------------------------------------------------------------------
IMPORTANT ASSUMPTIONS
--------------------------------------------------------------------------------

• Image is treated as point-data scalar image (1 value per voxel)
• Scalars are accessed using linear indexing: z*X*Y + y*X + x
• threshold defines "inside bone" (or other region of interest)
• dims[] must match the image dimensions

--------------------------------------------------------------------------------
PERFORMANCE NOTES
--------------------------------------------------------------------------------

• This avoids repeated bounds checking in user code
• Stop predicate allows early exit: critical for performance
• Consider replacing GetTuple1() with direct pointer access for speed
• Iterator uses integer arithmetic internally (Bresenham)

--------------------------------------------------------------------------------
EXTENSIONS
--------------------------------------------------------------------------------

This pattern can easily be extended to:

  • Accumulate values along a ray
  • Measure maximum thickness
  • Detect boundaries
  • Perform directional probing (as in your perpendicular sampling)
  • Compute intersection profiles

================================================================================


// Retrieve scalar data from the image
// This is assumed to be a single-component scalar field (e.g. intensity, mask).
vtkDataArray* scalars = image->GetPointData()->GetScalars();


// Define an early-termination predicate (lambda function)
// -------------------------------------------------------
//
// This function determines when the iterator should STOP.
// It is called for each voxel visited along the line.
//
auto stop = [&](const vtkVector3i& v)
    {
        // Convert 3D voxel coordinate (x,y,z) to linear index
        //
        // Layout assumes x varies fastest:
        //   idx = z*(dimX*dimY) + y*(dimX) + x
        //
        vtkIdType idx =
            static_cast<vtkIdType>(v[2]) * dims[0] * dims[1] +
            static_cast<vtkIdType>(v[1]) * dims[0] +
            v[0];

        // Stop condition:
        //
        // If scalar value falls below threshold,
        // we consider this voxel OUTSIDE bone (or region of interest),
        // and terminate iteration.
        //
        return scalars->GetTuple1(idx) < threshold;
    };


// Iterate over voxels along the line p1 -> p2
// -------------------------------------------------------
//
// • p1, p2 are in WORLD coordinates (vtkVector3d)
// • VoxelLine internally converts to index space
// • Iteration stops EARLY when stop() returns true
//
// The loop yields voxel indices (vtkVector3i):
//
for (auto v : VoxelLine(image, p1, p2, stop))
{
    // v is a voxel coordinate (x,y,z)

    // At this point:
    //   • voxel is guaranteed to be inside bounds
    //   • voxel satisfies stop condition == false
    //     → i.e., still inside the "bone" region

    // Example usage:
    //
    //  - accumulate positions
    //  - compute distances
    //  - track thickness
    //  - collect profile samples
}
*/

#include <vtkVector.h>
#include <vtkWeakPointer.h>

#include <cmath>
#include <functional>

class vtkImageData;

using Vec3i = vtkVector3i;
using Vec3d = vtkVector3d;
using StopPredicate = std::function<bool(const Vec3i&)>;

class VoxelLineIterator
{
public:
    VoxelLineIterator(vtkImageData* image,
                      const Vec3d& p1_world,
                      const Vec3d& p2_world,
                      StopPredicate stop = nullptr);

    VoxelLineIterator(vtkImageData* image,
                      const std::array<double, 3>& p1_world,
                      const std::array<double, 3>& p2_world,
                      StopPredicate stop = nullptr);    

    // STL-style iterator
    const Vec3i& operator*() const { return vcurrent; }
    const Vec3i* operator->() const { return &vcurrent; }

    VoxelLineIterator& operator++();
    bool operator==(const VoxelLineIterator& other) const;
    bool operator!=(const VoxelLineIterator& other) const;

    // factory for end iterator
    static VoxelLineIterator End();

    // world coordinate accessor
    Vec3d ToWorld() const;
    Vec3d ToIndex() const;
    vtkIdType ToFlatIndex() const;
    double Scalar() const;

    bool InBounds() const;
    bool IsInside(const double& threshold) const;
    bool IsAbove(const double& threshold) const;

    bool AdvanceToFirst(const double& threshold);

    static vtkVector3d ToIndex(vtkImageData* image, const vtkVector3d& p);

private:
    // internal state
    Vec3i vcurrent;
    Vec3i vend;

    int dx, dy, dz;
    int sx, sy, sz;
    int err1, err2;
    int stepCount = 0;
    int maxSteps = 0;
    bool finished = false;
    const int* dims = nullptr;

    vtkWeakPointer<vtkImageData> image;

    enum Axis { X, Y, Z } dominant;

    StopPredicate stopFunc;
};

class VoxelLine
{
public:
    // --- Constructor (vtkVector inputs) ---
    VoxelLine(vtkImageData* img,
              const Vec3d& p1,
              const Vec3d& p2,
              StopPredicate stop = nullptr)
        : image(img), vstart(p1), vend(p2), stopFunc(stop)
    {
    }

    // --- Constructor (std::array inputs) ---
    VoxelLine(vtkImageData* img,
              const std::array<double, 3>& p1,
              const std::array<double, 3>& p2,
              StopPredicate stop = nullptr)
        : VoxelLine(img,
                    Vec3d(p1[0], p1[1], p1[2]),
                    Vec3d(p2[0], p2[1], p2[2]),
                    stop)
    {
    }

    // --- STL-style begin() ---
    VoxelLineIterator begin() const
    {
        return VoxelLineIterator(image, vstart, vend, stopFunc);
    }

    // --- STL-style end() ---
    VoxelLineIterator end() const
    {
        return VoxelLineIterator::End();
    }

    double Length() const;

    Vec3d Direction() const;

    const Vec3d& Start() const { return vstart; }

    const Vec3d& End() const { return vend; }

private:
    vtkImageData* image = nullptr;
    Vec3d vstart;
    Vec3d vend;
    StopPredicate stopFunc;
};