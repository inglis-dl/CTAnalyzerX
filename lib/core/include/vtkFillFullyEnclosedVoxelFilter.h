#ifndef vtkFillFullyEnclosedVoxelFilter_h
#define vtkFillFullyEnclosedVoxelFilter_h

#include <vtkThreadedImageAlgorithm.h>

class vtkFillFullyEnclosedVoxelFilter : public vtkThreadedImageAlgorithm
{
public:
    static vtkFillFullyEnclosedVoxelFilter* New();
    vtkTypeMacro(vtkFillFullyEnclosedVoxelFilter, vtkThreadedImageAlgorithm);

    vtkSetMacro(ForegroundValue, unsigned char);
    vtkGetMacro(ForegroundValue, unsigned char);

    vtkSetMacro(BackgroundValue, unsigned char);
    vtkGetMacro(BackgroundValue, unsigned char);

    vtkGetMacro(FilledCount, int);

protected:
    vtkFillFullyEnclosedVoxelFilter();
    ~vtkFillFullyEnclosedVoxelFilter() override = default;

    void ThreadedRequestData(
        vtkInformation*,
        vtkInformationVector**,
        vtkInformationVector*,
        vtkImageData***,
        vtkImageData**,
        int extent[6],
        int threadId) override;

private:
    unsigned char ForegroundValue;
    unsigned char BackgroundValue;
    int FilledCount;
};

#endif
