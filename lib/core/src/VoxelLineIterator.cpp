#include "VoxelLineIterator.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkVectorOperators.h>
 
vtkVector3d VoxelLineIterator::ToIndex(vtkImageData* image, const vtkVector3d& p)
{
    const double* origin = image->GetOrigin();
    const double* spacing = image->GetSpacing();

    return vtkVector3d(
        (p[0] - origin[0]) / spacing[0],
        (p[1] - origin[1]) / spacing[1],
        (p[2] - origin[2]) / spacing[2]);
}

VoxelLineIterator::VoxelLineIterator(
    vtkImageData* img,
    const Vec3d& p1_world,
    const Vec3d& p2_world,
    StopPredicate stop)
    : image(img), stopFunc(stop)
{
    dims = image->GetDimensions();

    Vec3d p1 = ToIndex(image, p1_world);
    Vec3d p2 = ToIndex(image, p2_world);

    vcurrent = vtkVector3i(
        std::lround(p1[0]),
        std::lround(p1[1]),
        std::lround(p1[2]));

    vend = vtkVector3i(
        std::lround(p2[0]),
        std::lround(p2[1]),
        std::lround(p2[2]));

    Vec3i delta = vend - vcurrent;
    dx = std::abs(delta[0]);
    dy = std::abs(delta[1]);
    dz = std::abs(delta[2]);

    Vec3i step(
        (delta[0] >= 0) ? 1 : -1,
        (delta[1] >= 0) ? 1 : -1,
        (delta[2] >= 0) ? 1 : -1);

    sx = step[0];
    sy = step[1];
    sz = step[2];

    if (dx >= dy && dx >= dz)
    {
        dominant = X;
        err1 = 2 * dy - dx;
        err2 = 2 * dz - dx;
        maxSteps = dx;
    }
    else if (dy >= dx && dy >= dz)
    {
        dominant = Y;
        err1 = 2 * dx - dy;
        err2 = 2 * dz - dy;
        maxSteps = dy;
    }
    else
    {
        dominant = Z;
        err1 = 2 * dx - dz;
        err2 = 2 * dy - dz;
        maxSteps = dz;
    }

    stepCount = 0;
    finished = false;
}

VoxelLineIterator::VoxelLineIterator(
    vtkImageData* image,
    const std::array<double, 3>& p1_world,
    const std::array<double, 3>& p2_world,
    StopPredicate stop)
    : VoxelLineIterator(
        image,
        Vec3d(p1_world[0], p1_world[1], p1_world[2]),
        Vec3d(p2_world[0], p2_world[1], p2_world[2]),
        stop)
{
}

// --- increment ---
VoxelLineIterator& VoxelLineIterator::operator++()
{
    if (finished) return *this;

    if (vcurrent == vend || stepCount >= maxSteps)
    {
        finished = true;
        return *this;
    }

    switch (dominant)
    {
        case X:
        if (err1 > 0) { vcurrent[1] += sy; err1 -= 2 * dx; }
        if (err2 > 0) { vcurrent[2] += sz; err2 -= 2 * dx; }

        err1 += 2 * dy;
        err2 += 2 * dz;

        vcurrent[0] += sx;
        break;

        case Y:
        if (err1 > 0) { vcurrent[0] += sx; err1 -= 2 * dy; }
        if (err2 > 0) { vcurrent[2] += sz; err2 -= 2 * dy; }

        err1 += 2 * dx;
        err2 += 2 * dz;

        vcurrent[1] += sy;
        break;

        case Z:
        if (err1 > 0) { vcurrent[0] += sx; err1 -= 2 * dz; }
        if (err2 > 0) { vcurrent[1] += sy; err2 -= 2 * dz; }

        err1 += 2 * dx;
        err2 += 2 * dy;

        vcurrent[2] += sz;
        break;
    }

    ++stepCount;

    if (vcurrent[0] < 0 || vcurrent[1] < 0 || vcurrent[2] < 0 ||
        vcurrent[0] >= dims[0] ||
        vcurrent[1] >= dims[1] ||
        vcurrent[2] >= dims[2])
    {
        finished = true;
        return *this;
    }

    if (stopFunc && stopFunc(vcurrent))
    {
        finished = true;
        return *this;
    }

    return *this;
}

bool VoxelLineIterator::operator==(const VoxelLineIterator& other) const
{
    return finished == other.finished;
}

bool VoxelLineIterator::operator!=(const VoxelLineIterator& other) const
{
    return !(*this == other);
}

VoxelLineIterator VoxelLineIterator::End()
{
    VoxelLineIterator it(nullptr, Vec3d(0,0,0), Vec3d(0,0,0));
    it.finished = true;
    return it;
}

bool VoxelLineIterator::InBounds() const
{
    return vcurrent[0] >= 0 &&
        vcurrent[1] >= 0 &&
        vcurrent[2] >= 0 &&
        vcurrent[0] < dims[0] &&
        vcurrent[1] < dims[1] &&
        vcurrent[2] < dims[2];
}

bool VoxelLineIterator::IsAbove(const double& threshold) const
{
    return Scalar() >= threshold;
}

bool VoxelLineIterator::IsInside(const double& threshold) const
{
    return InBounds() && IsAbove(threshold);
}

bool VoxelLineIterator::AdvanceToFirst(const double& threshold)
{
    while (*this != End())
    {
        if (IsInside(threshold))
            return true;

        ++(*this);
    }
    return false;
}

vtkVector3d VoxelLineIterator::ToWorld() const
{
    const double* origin = image->GetOrigin();
    const double* spacing = image->GetSpacing();

    return vtkVector3d(
        origin[0] + vcurrent[0] * spacing[0],
        origin[1] + vcurrent[1] * spacing[1],
        origin[2] + vcurrent[2] * spacing[2]);
}

vtkVector3d VoxelLineIterator::ToIndex() const
{
    const double* origin = image->GetOrigin();
    const double* spacing = image->GetSpacing();

    return vtkVector3d(
        (vcurrent[0] - origin[0]) / spacing[0],
        (vcurrent[1] - origin[1]) / spacing[1],
        (vcurrent[2] - origin[2]) / spacing[2]);
}

vtkIdType VoxelLineIterator::ToFlatIndex() const
{
    return static_cast<vtkIdType>(vcurrent[2]) * dims[1] * dims[0] +
        static_cast<vtkIdType>(vcurrent[1]) * dims[0] +
        vcurrent[0];
}

double VoxelLineIterator::Scalar() const
{
    auto scalars = image->GetPointData()->GetScalars();
    return scalars->GetTuple1(ToFlatIndex());
}\


double VoxelLine::Length() const
{
    return (vend - vstart).Norm();
}

Vec3d VoxelLine::Direction() const
{
    Vec3d d = vend - vstart;
    d.Normalize();
    return d;
}
