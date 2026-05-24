#include "FillFullyEnclosedVoxelImageFilter.h"

#include <itkImageRegionIterator.h>
#include <itkConstNeighborhoodIterator.h>

namespace itk
{
	template <typename TImage>
	FillFullyEnclosedVoxelImageFilter<TImage>::FillFullyEnclosedVoxelImageFilter()
	{
		m_ForegroundValue = NumericTraits<PixelType>::OneValue();
		m_BackgroundValue = NumericTraits<PixelType>::ZeroValue();
	}

	template <typename TImage>
	void FillFullyEnclosedVoxelImageFilter<TImage>::DynamicThreadedGenerateData(
			const RegionType& outputRegionForThread)
	{
		const typename ImageType::ConstPointer input = this->GetInput();
		typename ImageType::Pointer output = this->GetOutput();

		typename NeighborhoodIterator<ImageType>::RadiusType radius;
		radius.Fill(1);

		ConstNeighborhoodIterator<ImageType> neighIt(radius, input, outputRegionForThread);
		ImageRegionIterator<ImageType> outIt(output, outputRegionForThread);

		const unsigned int centerIndex = neighIt.GetCenterNeighborhoodIndex();

		for (neighIt.GoToBegin(), outIt.GoToBegin();
			 !neighIt.IsAtEnd();
			 ++neighIt, ++outIt)
		{
			PixelType centerValue = neighIt.GetCenterPixel();
			PixelType newValue = centerValue;

			if (centerValue == m_BackgroundValue)
			{
				bool allForeground = true;

				for (unsigned int i = 0; i < neighIt.Size(); ++i)
				{
					if (i == centerIndex)
						continue;

					if (neighIt.GetPixel(i) != m_ForegroundValue)
					{
						allForeground = false;
						break;
					}
				}

				if (allForeground)
				{
					newValue = m_ForegroundValue;
				}
			}

			outIt.Set(newValue);
		}
	}

} // namespace itk
