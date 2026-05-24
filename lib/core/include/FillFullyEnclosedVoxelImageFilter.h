#pragma once

#include "itkImageToImageFilter.h"
#include "itkNeighborhoodIterator.h"

namespace itk
{

	template <typename TImage>
	class FillFullyEnclosedVoxelImageFilter
		: public ImageToImageFilter<TImage, TImage>
	{
	public:
		ITK_DISALLOW_COPY_AND_MOVE(FillFullyEnclosedVoxelImageFilter);

		using Self = FillFullyEnclosedVoxelImageFilter;
		using Superclass = ImageToImageFilter<TImage, TImage>;
		using Pointer = SmartPointer<Self>;
		using ConstPointer = SmartPointer<const Self>;

		itkNewMacro(Self);
		itkTypeMacro(FillFullyEnclosedVoxelImageFilter, ImageToImageFilter);

		using ImageType = TImage;
		using PixelType = typename ImageType::PixelType;
		using RegionType = typename ImageType::RegionType;

		/** Set/Get foreground value */
		itkSetMacro(ForegroundValue, PixelType);
		itkGetMacro(ForegroundValue, PixelType);

		/** Set/Get background value */
		itkSetMacro(BackgroundValue, PixelType);
		itkGetMacro(BackgroundValue, PixelType);

	protected:
		FillFullyEnclosedVoxelImageFilter();
		~FillFullyEnclosedVoxelImageFilter() override = default;

		void DynamicThreadedGenerateData(const RegionType& outputRegionForThread) override;

	private:
		PixelType m_ForegroundValue;
		PixelType m_BackgroundValue;
	};

} // namespace itk

#ifndef ITK_MANUAL_INSTANTIATION
#include "FillFullyEnclosedVoxelImageFilter.hxx"
#endif

