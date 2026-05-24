#include "vtkFillFullyEnclosedVoxelFilter.h"

#include <vtkObjectFactory.h>
#include <vtkImageData.h>

vtkStandardNewMacro(vtkFillFullyEnclosedVoxelFilter);

vtkFillFullyEnclosedVoxelFilter::vtkFillFullyEnclosedVoxelFilter()
{
    this->ForegroundValue = 255;
    this->BackgroundValue = 0;
    this->SetNumberOfInputPorts(1);
    this->SetNumberOfOutputPorts(1);
    this->FilledCount = 0;
}

void vtkFillFullyEnclosedVoxelFilter::ThreadedRequestData(
    vtkInformation*,
    vtkInformationVector**,
    vtkInformationVector*,
    vtkImageData*** inData,
    vtkImageData** outData,
    int extent[6],
    int vtkNotUsed(threadId))
{
    vtkImageData* input = inData[0][0];
    vtkImageData* output = outData[0];

    int wholeExtent[6];
    input->GetExtent(wholeExtent);

    const int nx = wholeExtent[1] - wholeExtent[0] + 1;
    const int ny = wholeExtent[3] - wholeExtent[2] + 1;

    const auto* inPtr =
        static_cast<const unsigned char*>(input->GetScalarPointer());
    auto* outPtr =
        static_cast<unsigned char*>(output->GetScalarPointer());

    // Process only assigned thread extent
    for (int k = extent[4]; k <= extent[5]; ++k)
        for (int j = extent[2]; j <= extent[3]; ++j)
            for (int i = extent[0]; i <= extent[1]; ++i)
            {
                vtkIdType flat =
                    static_cast<vtkIdType>(k - wholeExtent[4]) * nx * ny +
                    static_cast<vtkIdType>(j - wholeExtent[2]) * nx +
                    (i - wholeExtent[0]);

                unsigned char center = inPtr[flat];

                // Default: copy input
                unsigned char newValue = center;

                if (center == this->BackgroundValue)
                {
                    bool allForeground = true;

                    for (int dz = -1; dz <= 1 && allForeground; ++dz)
                        for (int dy = -1; dy <= 1 && allForeground; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                if (dx == 0 && dy == 0 && dz == 0)
                                    continue;

                                int ni = i + dx;
                                int nj = j + dy;
                                int nk = k + dz;

                                // Outside image → NOT enclosed
                                if (ni < wholeExtent[0] || ni > wholeExtent[1] ||
                                    nj < wholeExtent[2] || nj > wholeExtent[3] ||
                                    nk < wholeExtent[4] || nk > wholeExtent[5])
                                {
                                    allForeground = false;
                                    break;
                                }

                                vtkIdType nflat =
                                    static_cast<vtkIdType>(nk - wholeExtent[4]) * nx * ny +
                                    static_cast<vtkIdType>(nj - wholeExtent[2]) * nx +
                                    (ni - wholeExtent[0]);

                                if (inPtr[nflat] != this->ForegroundValue)
                                {
                                    allForeground = false;
                                    break;
                                }
                            }

                    if (allForeground)
                    {
                        newValue = this->ForegroundValue;
                        this->FilledCount++;
                    }
                }

                outPtr[flat] = newValue;
            }
}
