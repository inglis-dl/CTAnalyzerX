/*=========================================================================

  Program:
  Module:    vtkImageCoordinateWidget.cxx
  Language:  C++

  Author: Dean Inglis <inglisd AT gmail DOT com>

=========================================================================*/
#include <vtkImageCoordinateWidget.h>

// VTK includes
#include <vtkActor.h>
#include <vtkAssemblyNode.h>
#include <vtkAssemblyPath.h>
#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkHomogeneousTransform.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPropPicker.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>

vtkStandardNewMacro(vtkImageCoordinateWidget);
vtkCxxSetObjectMacro(vtkImageCoordinateWidget, UserTransform, vtkHomogeneousTransform);

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
vtkImageCoordinateWidget::vtkImageCoordinateWidget()
{
	this->EventCallbackCommand->SetCallback(
		vtkImageCoordinateWidget::ProcessEvents);
	this->EventCallbackCommand->SetPassiveObserver(1);  // get events first

	this->State = vtkImageCoordinateWidget::Start;
	this->CursoringMode = vtkImageCoordinateWidget::Continuous;

	this->CurrentCursorPosition[0] = 0;
	this->CurrentCursorPosition[1] = 0;
	this->CurrentCursorPosition[2] = 0;
	this->CurrentImageValue.clear();
	this->MessageString = "NA";

	this->PropCollection = vtkSmartPointer<vtkPropCollection>::New();
	this->ImageData = 0;
	this->Picker = 0;
	this->UserTransform = 0;

	this->OutPD = vtkSmartPointer<vtkPointData>::New();
	this->CachedNumScalarComponents = 0;
	this->CachedImageLength = 0.0;
	this->CachedScalarType = VTK_VOID;
	this->CachedScalars = nullptr;

	this->SetPicker(vtkSmartPointer<vtkPropPicker>::New());
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
vtkImageCoordinateWidget::~vtkImageCoordinateWidget()
{
	this->RemoveAllProps();
	this->SetEnabled(0);
	this->SetUserTransform(0);
	this->SetInputData(0);
	this->SetPicker(0);
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::SetPicker(vtkAbstractPropPicker* picker)
{
	if (this->Picker != picker)
	{
		// to avoid destructor recursion
		vtkAbstractPropPicker* temp = this->Picker;
		this->Picker = picker;
		if (temp)
		{
			temp->UnRegister(this);
		}
		if (this->Picker)
		{
			this->Picker->Register(this);
			this->Picker->PickFromListOn();
			for (int i = 0; i < this->GetNumberOfProps(); ++i)
			{
				this->Picker->AddPickList(this->GetNthProp(i));
			}
		}
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::AddViewProp(vtkProp* prop)
{
	if (prop)
	{
		this->PropCollection->AddItem(prop);
		if (this->Picker) this->Picker->AddPickList(prop);
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::RemoveProp(vtkProp* prop)
{
	if (prop)
	{
		this->PropCollection->RemoveItem(prop);
		if (this->Picker) this->Picker->DeletePickList(prop);
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::RemoveAllProps()
{
	this->PropCollection->RemoveAllItems();
	if (this->Picker) this->Picker->InitializePickList();
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::RemoveProps(vtkPropCollection* props)
{
	if (!props) return;
	vtkProp* aProp;
	vtkCollectionSimpleIterator pit;
	for (props->InitTraversal(pit);
		(aProp = props->GetNextProp(pit));)
	{
		this->RemoveProp(aProp);
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::HasProp(vtkProp* prop) const
{
	return this->PropCollection->IsItemPresent(prop);
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetNumberOfProps() const
{
	return this->PropCollection->GetNumberOfItems();
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
vtkProp* vtkImageCoordinateWidget::GetNthProp(int id) const
{
	return vtkProp::SafeDownCast(this->PropCollection->GetItemAsObject(id));
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::SetCursoringMode(int mode)
{
	this->CursoringMode =
		mode < vtkImageCoordinateWidget::Discrete ?
		vtkImageCoordinateWidget::Discrete :
		mode > vtkImageCoordinateWidget::Continuous ?
		vtkImageCoordinateWidget::Continuous : mode;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::SetInputData(vtkDataSet* input)
{
	this->ImageData = vtkImageData::SafeDownCast(input);

	// Reset cache defaults
	this->CachedNumScalarComponents = 0;
	this->CachedImageLength = 0.0;
	this->CachedScalarType = VTK_VOID;
	this->CachedScalars = nullptr;

	if (!this->ImageData)
	{
		// If input is null, ensure OutPD is cleared
		this->OutPD->Initialize();
		return;
	}

	// basic caches
	this->ImageData->GetSpacing(this->CachedSpacing);
	this->ImageData->GetOrigin(this->CachedOrigin);
	this->ImageData->GetExtent(this->CachedExtent);
	this->ImageData->GetIncrements(this->CachedIncrements);

	// Cache number of scalar components and image length once
	this->CachedNumScalarComponents = this->ImageData->GetNumberOfScalarComponents();
	this->CachedImageLength = this->ImageData->GetLength();

	// Pre-allocate OutPD for interpolation using the source point data
	vtkPointData* pd = this->ImageData->GetPointData();
	if (pd)
	{
		vtkDataArray* scalars = pd->GetScalars();
		if (scalars)
		{
			this->CachedScalars = scalars; // vtkSmartPointer will manage refcount
			this->CachedScalarType = scalars->GetDataType();
		}
		// Use the point data to set up OutPD's scalars/type/size once
		this->OutPD->InterpolateAllocate(pd, 1, 1);
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::SetEnabled(int enabling)
{
	// we have to have a picker for the cursor to work
	if (!this->Interactor || !this->Picker)
	{
		return;
	}

	if (enabling)  // ----------------------------------------------------------
	{
		vtkDebugMacro(<< "Enabling vtkImageCoordinateWidget");

		if (this->Enabled)  // already enabled, just return
		{
			return;
		}

		if (!this->CurrentRenderer)
		{
			this->SetCurrentRenderer(this->Interactor->FindPokedRenderer(
				this->Interactor->GetLastEventPosition()[0],
				this->Interactor->GetLastEventPosition()[1]));
			if (!this->CurrentRenderer)
			{
				return;
			}
		}

		this->Enabled = 1;

		// we have to honour this ivar: it could be that this->Interaction was
		// set to off when we were disabled

		this->Interactor->AddObserver(vtkCommand::MouseMoveEvent,
			this->EventCallbackCommand, this->Priority);
		this->Interactor->AddObserver(vtkCommand::EnterEvent,
			this->EventCallbackCommand, this->Priority);
		this->Interactor->AddObserver(vtkCommand::LeaveEvent,
			this->EventCallbackCommand, this->Priority);

		this->InvokeEvent(vtkCommand::EnableEvent, 0);

		this->EventCallbackCommand->SetAbortFlag(1);
		this->StartInteraction();
		this->InvokeEvent(vtkCommand::StartInteractionEvent, 0);
		this->Interactor->Render();
		this->State = vtkImageCoordinateWidget::Cursoring;
	}
	else  // disabling----------------------------------------------------------
	{
		vtkDebugMacro(<< "Disabling vtkImageCoordinateWidget");

		if (!this->Enabled)  // already disabled, just return
		{
			return;
		}

		this->State = vtkImageCoordinateWidget::Start;

		this->EventCallbackCommand->SetAbortFlag(1);
		this->EndInteraction();
		this->InvokeEvent(vtkCommand::EndInteractionEvent, 0);
		this->Interactor->Render();

		this->Enabled = 0;

		// don't listen for events any more
		this->Interactor->RemoveObserver(this->EventCallbackCommand);

		this->InvokeEvent(vtkCommand::DisableEvent, 0);
		this->SetCurrentRenderer(0);
	}

	this->Interactor->Render();
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::ProcessEvents(
	vtkObject* vtkNotUsed(object), unsigned long event,
	void* clientdata, void* vtkNotUsed(calldata))
{
	vtkImageCoordinateWidget* self =
		reinterpret_cast<vtkImageCoordinateWidget*>(clientdata);

	if (event == vtkCommand::MouseMoveEvent)
	{
		self->OnMouseMove();
		return;
	}
	if (event == vtkCommand::EnterEvent)
	{
		self->State = vtkImageCoordinateWidget::Cursoring;
	}
	else if (event == vtkCommand::LeaveEvent)
	{
		self->State = vtkImageCoordinateWidget::Outside;
		self->MessageString = "Off Image";
	}
	self->EventCallbackCommand->SetAbortFlag(1);
	self->InvokeEvent(vtkCommand::InteractionEvent, 0);
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::OnMouseMove()
{
	// See whether we're active
	//
	if (this->State == vtkImageCoordinateWidget::Outside ||
		this->State == vtkImageCoordinateWidget::Start)
	{
		return;
	}

	int X, Y;
	this->Interactor->GetLastEventPosition(X, Y);

	if (this->State == vtkImageCoordinateWidget::Cursoring)
	{
		this->UpdateCursor(X, Y);
	}

	// Interact, if desired
	//
	this->EventCallbackCommand->SetAbortFlag(1);
	this->InvokeEvent(vtkCommand::InteractionEvent, 0);
	this->Interactor->Render();
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorPosition(
	double& x, double& y, double& z) const
{
	if (this->State != vtkImageCoordinateWidget::Cursoring)
	{
		return 0;
	}

	x = this->CurrentCursorPosition[0];
	y = this->CurrentCursorPosition[1];
	z = this->CurrentCursorPosition[2];

	return 1;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorDataN(double* v, const int& components) const
{
	if (this->State != vtkImageCoordinateWidget::Cursoring)
	{
		return 0;
	}

	const size_t currSize = this->CurrentImageValue.size();
	for (size_t c = 0; c < components; ++c)
	{
		if (c < currSize)
		{
			v[c] = this->CurrentImageValue[c];
		}
		else
		{
			// anything out of range gets the max double value
			v[c] = VTK_DOUBLE_MAX;
		}
	}

	return 1;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorData1(double& v1) const
{
	double v[1];
	const int retVal = this->GetCursorDataN(v, 1);
	if (retVal)
	{
		v1 = v[0];
	}

	return retVal;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorData2(double& v1, double& v2) const
{
	double v[2];
	const int retVal = this->GetCursorDataN(v, 2);
	if (retVal)
	{
		v1 = v[0];
		v2 = v[1];
	}

	return retVal;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorData3(
	double& v1, double& v2, double& v3) const
{
	double v[3];
	const int retVal = this->GetCursorDataN(v, 3);
	if (retVal)
	{
		v1 = v[0];
		v2 = v[1];
		v3 = v[2];
	}

	return retVal;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorData4(
	double& v1, double& v2, double& v3, double& v4) const
{
	double v[4];
	const int retVal = this->GetCursorDataN(v, 4);
	if (retVal)
	{
		v1 = v[0];
		v2 = v[1];
		v3 = v[2];
		v4 = v[3];
	}

	return retVal;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
int vtkImageCoordinateWidget::GetCursorData9(
	double& v1, double& v2, double& v3, double& v4,
	double& v5, double& v6, double& v7, double& v8, double& v9) const
{
	double v[9];
	const int retVal = this->GetCursorDataN(v, 9);
	if (retVal)
	{
		v1 = v[0];
		v2 = v[1];
		v3 = v[2];
		v4 = v[3];
		v5 = v[4];
		v6 = v[5];
		v7 = v[6];
		v8 = v[7];
		v9 = v[8];
	}

	return retVal;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::UpdateMessageString()
{
	// See whether we're active
	//
	if (this->State == vtkImageCoordinateWidget::Outside ||
		this->State == vtkImageCoordinateWidget::Start)
	{
		return;
	}
	int X, Y;
	this->Interactor->GetLastEventPosition(X, Y);
	this->UpdateCursor(X, Y);
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
bool vtkImageCoordinateWidget::GetCursorPosition(int X, int Y, double& x, double& y, double& z)
{
	if (this->GetNumberOfProps() == 0) return false;

	// try to use the actor's image data if it is a single vtkImageSlice
	if (!this->ImageData)
	{
		vtkImageSlice* actor =
			vtkImageSlice::SafeDownCast(this->GetNthProp(0));
		if (!actor || !actor->GetMapper() ||
			!(this->ImageData = actor->GetMapper()->GetInput()))
		{
			return false;
		}
		actor->GetMapper()->Update();
	}

	this->Picker->Pick(X, Y, 0.0, this->CurrentRenderer);
	vtkAssemblyPath* path = this->Picker->GetPath();

	vtkProp* pickedProp = nullptr;
	if (path)
	{
		// Deal with the possibility that we may be using a shared picker
		vtkCollectionSimpleIterator sit;
		path->InitTraversal(sit);
		vtkAssemblyNode* node;
		for (int i = 0; i < path->GetNumberOfItems(); ++i)
		{
			node = path->GetNextNode(sit);
			pickedProp = node->GetViewProp();
			if (this->HasProp(pickedProp))
			{
				break;
			}
		}
	}

	if (!pickedProp) return false;

	double q[3];
	this->Picker->GetPickPosition(q);
	const double* bounds = pickedProp->GetBounds();

	if (bounds[0] == bounds[1])       // YZ
	{
		q[0] = bounds[0];
	}
	else if (bounds[2] == bounds[3])  // XZ
	{
		q[1] = bounds[2];
	}
	else if (bounds[4] == bounds[5])  // XY
	{
		q[2] = bounds[4];
	}

	if (this->UserTransform)
	{
		this->UserTransform->TransformPoint(q, q);
	}
	x = q[0];
	y = q[1];
	z = q[2];

	return true;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::UpdateCursor(int X, int Y)
{
	this->MessageString = "Off Image";

	if (this->GetNumberOfProps() == 0) return;

	// try to use the actor's image data if it is a single vtkImageSlice
	if (!this->ImageData)
	{
		vtkImageSlice* actor =
			vtkImageSlice::SafeDownCast(this->GetNthProp(0));
		// Do not assign ImageData directly here; use SetInputData so caches are initialized
		if (!actor || !actor->GetMapper() || !(actor->GetMapper()->GetInput()))
		{
			return;
		}
		actor->GetMapper()->Update();

		// Populate ImageData and all cached quantities via SetInputData
		this->SetInputData(actor->GetMapper()->GetInput());
	}

	// We're going to be extracting values with GetScalarComponentAsDouble(),
	// we might as well make sure that the data is there.  If the data is
	// up to date already, this call doesn't cost very much.  If we don't make
	// this call and the data is not up to date, the GetScalar() call will
	// cause a segfault.
	this->Picker->Pick(X, Y, 0.0, this->CurrentRenderer);
	vtkAssemblyPath* path = this->Picker->GetPath();

	vtkProp* pickedProp = nullptr;
	if (path)
	{
		// Deal with the possibility that we may be using a shared picker
		vtkCollectionSimpleIterator sit;
		path->InitTraversal(sit);
		vtkAssemblyNode* node;
		for (int i = 0; i < path->GetNumberOfItems(); ++i)
		{
			node = path->GetNextNode(sit);
			pickedProp = node->GetViewProp();
			if (this->HasProp(pickedProp))
			{
				break;
			}
		}
	}

	if (!pickedProp) return;

	double q[3];
	this->Picker->GetPickPosition(q);
	const double* bounds = pickedProp->GetBounds();

	if (bounds[0] == bounds[1])       // YZ
	{
		q[0] = bounds[0];
	}
	else if (bounds[2] == bounds[3])  // XZ
	{
		q[1] = bounds[2];
	}
	else if (bounds[4] == bounds[5])  // XY
	{
		q[2] = bounds[4];
	}

	if (this->UserTransform)
	{
		this->UserTransform->TransformPoint(q, q);
	}

	if (this->CursoringMode == vtkImageCoordinateWidget::Continuous)
	{
		this->UpdateContinuousCursor(q);
	}
	else
	{
		this->UpdateDiscreteCursor(q);
	}

	this->MessageString = "Location: (";
	for (int i = 0; i < 3; ++i)
	{
		this->MessageString +=
			vtkVariant(this->CurrentCursorPosition[i]).ToString();
		if (i < 2)
			this->MessageString += ", ";
	}
	this->MessageString += ")";

	const size_t components = this->CurrentImageValue.size();
	if (0 < components)
	{
		this->MessageString += " Value: ";
		// if we only have one component then do not use round brackets
		if (1 == components)
		{
			this->MessageString +=
				vtkVariant(this->CurrentImageValue[0]).ToString();
		}
		else
		{
			this->MessageString += "(";
			for (size_t c = 0; c < components; ++c)
			{
				this->MessageString +=
					vtkVariant(this->CurrentImageValue[c]).ToString();
				if ((c + 1) < components) this->MessageString += ", ";
			}
			this->MessageString += ")";
		}
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
// Updated UpdateContinuousCursor: use cached values and avoid repeated InterpolateAllocate
void vtkImageCoordinateWidget::UpdateContinuousCursor(double* q)
{
	// empty the current image value
	this->CurrentImageValue.clear();

	this->CurrentCursorPosition[0] = q[0];
	this->CurrentCursorPosition[1] = q[1];
	this->CurrentCursorPosition[2] = q[2];

	vtkPointData* pd = this->ImageData->GetPointData();
	if (!pd) return;

	// No repeated InterpolateAllocate here - it was done once in SetInputData.
	// Use cached image length for tolerance computation. Fallback to ImageData->GetLength()
	double tol2 = this->CachedImageLength ? this->CachedImageLength * this->CachedImageLength / 1000.0
		: this->ImageData->GetLength() ? this->ImageData->GetLength() * this->ImageData->GetLength() / 1000.0
		: 0.001;

	// Find the cell that contains q and get it
	//
	int subId;
	double pcoords[3], weights[8];
	vtkCell* cell = this->ImageData->FindAndGetCell(
		q, 0, -1, tol2, subId, pcoords, weights);
	if (cell)
	{
		// Interpolate the point data (OutPD has been preallocated)
		this->OutPD->InterpolatePoint(pd, 0, cell->PointIds, weights);

		auto scalars = this->OutPD->GetScalars();
		if (scalars == nullptr) return;

		// Use cached component count when valid; otherwise fall back to current scalars
		int components = this->CachedNumScalarComponents ? this->CachedNumScalarComponents
			: scalars->GetNumberOfComponents();

		const double* tuple = scalars->GetTuple(0);

		this->CurrentImageValue.resize(components);
		for (int c = 0; c < components; ++c)
		{
			this->CurrentImageValue[c] = tuple[c];
		}
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
bool vtkImageCoordinateWidget::GetDiscreteCoordinate(double* q, int* iq)
{
	// vtkImageData will find the nearest implicit point to q
	//
	vtkIdType ptId = this->ImageData->FindPoint(q);

	if (ptId == -1) return false;

	int ext[6];
	this->ImageData->GetExtent(ext);

	double ijk[3];
	this->ImageData->TransformPhysicalPointToContinuousIndex(q, ijk);
	for (int i = 0; i < 3; i++)
	{
		iq[i] = int(vtkMath::Round(ijk[i]));
		iq[i] = std::max<int>(ext[2 * i], std::min<int>(iq[i], ext[2 * i + 1]));
	}

	return true;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::UpdateDiscreteCursor(double* q)
{
	// empty the current image value
	this->CurrentImageValue.clear();

	// vtkImageData will find the nearest implicit point to q
	vtkIdType ptId = this->ImageData->FindPoint(q);
	if (ptId == -1) return;

	int ext[6];
	if (this->CachedNumScalarComponents > 0)
	{
		std::memcpy(ext, this->CachedExtent, 6 * sizeof(int));
	}
	else
	{
		this->ImageData->GetExtent(ext);
	}

	double ijk[3];
	this->ImageData->TransformPhysicalPointToContinuousIndex(q, ijk);
	for (int i = 0; i < 3; i++)
	{
		int iq = int(vtkMath::Round(ijk[i]));
		this->CurrentCursorPosition[i] = std::max<int>(ext[2 * i], std::min<int>(iq, ext[2 * i + 1]));
	}

	// integer voxel coordinates
	const int x = static_cast<int>(this->CurrentCursorPosition[0]);
	const int y = static_cast<int>(this->CurrentCursorPosition[1]);
	const int z = static_cast<int>(this->CurrentCursorPosition[2]);

	// Fast path: use cached scalars + increments if available
	if (this->CachedNumScalarComponents > 0 && this->CachedScalars)
	{
		// compute point id using cached extent/increments if present, otherwise compute increments now
		vtkIdType inc[3];
		if (this->CachedIncrements[0] || this->CachedIncrements[1] || this->CachedIncrements[2])
		{
			inc[0] = this->CachedIncrements[0];
			inc[1] = this->CachedIncrements[1];
			inc[2] = this->CachedIncrements[2];
		}
		else
		{
			this->ImageData->GetIncrements(inc);
		}

		// compute (i,j,k) relative to extent minima
		const int i_rel = x - ext[0];
		const int j_rel = y - ext[2];
		const int k_rel = z - ext[4];

		const vtkIdType pointId = static_cast<vtkIdType>(i_rel) * inc[0] +
			static_cast<vtkIdType>(j_rel) * inc[1] +
			static_cast<vtkIdType>(k_rel) * inc[2];

		// read tuple once
		const double* tuple = this->CachedScalars->GetTuple(pointId);
		if (!tuple) return;

		this->CurrentImageValue.resize(this->CachedNumScalarComponents);
		for (int c = 0; c < this->CachedNumScalarComponents; ++c)
		{
			this->CurrentImageValue[c] = tuple[c];
		}
		return;
	}

	// Fallback: original safe per-component read (works if caches not populated)
	const int components = this->ImageData->GetNumberOfScalarComponents();
	this->CurrentImageValue.resize(components);
	for (int c = 0; c < components; ++c)
	{
		this->CurrentImageValue[c] =
			this->ImageData->GetScalarComponentAsDouble(x, y, z, c);
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void vtkImageCoordinateWidget::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
	os << indent << "CursoringMode: " << this->CursoringMode << endl;
}
