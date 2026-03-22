#include "vtkTransformWidget.h"
#include "vtkTransformRepresentation.h"
#include "vtkCallbackCommand.h"
#include "vtkCommand.h"
#include "vtkEvent.h"
#include "vtkHandleWidget.h"
#include "vtkObjectFactory.h"
#include "vtkPointHandleRepresentation3D.h"
#include "vtkRenderWindow.h"
#include "vtkRenderWindowInteractor.h"
#include "vtkRendererCollection.h"
#include "vtkWidgetCallbackMapper.h"
#include "vtkWidgetEvent.h"
#include "vtkWidgetEventTranslator.h"
#include "vtkTransform.h"

#include "vtkTimeStamp.h"
#include "vtkTimerLog.h"
#include <vtkMatrix4x4.h>
#include <vtkSmartPointer.h>

#include <iostream>

vtkStandardNewMacro(vtkTransformWidget);

// Define ResetEvent and ValueChangedEvent as user events (avoid colliding with built-in events)
const unsigned long vtkTransformWidget::ResetEvent = vtkCommand::UserEvent + 1;
const unsigned long vtkTransformWidget::ValueChangedEvent = vtkCommand::UserEvent + 2;

//------------------------------------------------------------------------------
// Configuration setters/getters
void vtkTransformWidget::SetValueChangedInterval(int ms)
{
	if (ms < 0)
	{
		ms = 0;
	}
	this->ValueChangedIntervalMs = ms;
}

int vtkTransformWidget::GetValueChangedInterval() const
{
	return this->ValueChangedIntervalMs;
}

void vtkTransformWidget::SetReportValueDuringInteraction(vtkTypeBool val)
{
	this->ReportValueDuringInteraction = val;
}

vtkTypeBool vtkTransformWidget::GetReportValueDuringInteraction() const
{
	return this->ReportValueDuringInteraction;
}

//------------------------------------------------------------------------------
// New: Reset() helper (preserve backward compatibility if other code calls widget->Reset())
void vtkTransformWidget::Reset()
{
	this->InvokeEvent(vtkTransformWidget::ResetEvent, nullptr);
}

//------------------------------------------------------------------------------
// Constructor
vtkTransformWidget::vtkTransformWidget()
{
	this->WidgetState = vtkTransformWidget::Start;
	this->ManagesCursor = 1;
	this->CurrentHandle = 0; // 0 = center, 1 = X, 2 = Y, 3 = Z

	// Default reporting/throttle values (tunable)
	this->ValueChangedIntervalMs = 30; // default ~33 updates/sec
	this->ReportValueDuringInteraction = 1;

	// Use VTK timestamping instead of std::chrono
	this->LastValueEventTime.Modified();
	this->LastValueEventTimeSec = -1.0; // negative signals "never emitted" so first move will notify

	// The widgets for moving the end points. They observe this widget (i.e.,
	// this widget is the parent to the handles).
	this->OriginWidget = vtkHandleWidget::New();
	this->OriginWidget->SetPriority(this->Priority - 0.01);
	this->OriginWidget->SetParent(this);
	this->OriginWidget->ManagesCursorOff();

	this->SelectionXWidget = vtkHandleWidget::New();
	this->SelectionXWidget->SetPriority(this->Priority - 0.01);
	this->SelectionXWidget->SetParent(this);
	this->SelectionXWidget->ManagesCursorOff();

	this->SelectionYWidget = vtkHandleWidget::New();
	this->SelectionYWidget->SetPriority(this->Priority - 0.01);
	this->SelectionYWidget->SetParent(this);
	this->SelectionYWidget->ManagesCursorOff();

	this->SelectionZWidget = vtkHandleWidget::New();
	this->SelectionZWidget->SetPriority(this->Priority - 0.01);
	this->SelectionZWidget->SetParent(this);
	this->SelectionZWidget->ManagesCursorOff();

	// Define widget events
	this->CallbackMapper->SetCallbackMethod(vtkCommand::LeftButtonPressEvent, vtkWidgetEvent::Select,
	  this, vtkTransformWidget::SelectAction);
	this->CallbackMapper->SetCallbackMethod(vtkCommand::LeftButtonReleaseEvent,
	  vtkWidgetEvent::EndSelect, this, vtkTransformWidget::EndSelectAction);
	this->CallbackMapper->SetCallbackMethod(
	  vtkCommand::MouseMoveEvent, vtkWidgetEvent::Move, this, vtkTransformWidget::MoveAction);
}

//------------------------------------------------------------------------------
// Destructor
vtkTransformWidget::~vtkTransformWidget()
{
	this->OriginWidget->Delete();
	this->SelectionXWidget->Delete();
	this->SelectionYWidget->Delete();
	this->SelectionZWidget->Delete();
}

//------------------------------------------------------------------------------
void vtkTransformWidget::SetEnabled(int enabling)
{
	// We defer enabling the handles until the selection process begins
	if (enabling)
	{

		if (!this->CurrentRenderer)
		{
			int X = this->Interactor->GetEventPosition()[0];
			int Y = this->Interactor->GetEventPosition()[1];

			this->SetCurrentRenderer(this->Interactor->FindPokedRenderer(X, Y));

			if (this->CurrentRenderer == nullptr)
			{
				return;
			}
		}

		// Don't actually turn these on until cursor is near the end points or the line.
		this->CreateDefaultRepresentation();
		vtkHandleRepresentation* originRep =
			reinterpret_cast<vtkHandleRepresentation*>(
			reinterpret_cast<vtkTransformRepresentation*>(this->WidgetRep)->GetOriginRepresentation());
		originRep->SetRenderer(this->CurrentRenderer);
		this->OriginWidget->SetRepresentation(originRep);
		this->OriginWidget->SetInteractor(this->Interactor);

		vtkHandleRepresentation* selectionRep =
			reinterpret_cast<vtkHandleRepresentation*>(
			reinterpret_cast<vtkTransformRepresentation*>(this->WidgetRep)
			->GetSelectionXRepresentation());
		selectionRep->SetRenderer(this->CurrentRenderer);
		this->SelectionXWidget->SetRepresentation(selectionRep);
		this->SelectionXWidget->SetInteractor(this->Interactor);

		selectionRep =
			reinterpret_cast<vtkHandleRepresentation*>(
			reinterpret_cast<vtkTransformRepresentation*>(this->WidgetRep)
			->GetSelectionYRepresentation());
		selectionRep->SetRenderer(this->CurrentRenderer);
		this->SelectionYWidget->SetRepresentation(selectionRep);
		this->SelectionYWidget->SetInteractor(this->Interactor);

		selectionRep =
			reinterpret_cast<vtkHandleRepresentation*>(
			reinterpret_cast<vtkTransformRepresentation*>(this->WidgetRep)
			->GetSelectionZRepresentation());
		selectionRep->SetRenderer(this->CurrentRenderer);
		this->SelectionZWidget->SetRepresentation(selectionRep);
		this->SelectionZWidget->SetInteractor(this->Interactor);

		// We do this step first because it sets the CurrentRenderer
		this->Superclass::SetEnabled(enabling);
	}
	else
	{
		this->OriginWidget->SetEnabled(0);
		this->SelectionXWidget->SetEnabled(0);
		this->SelectionYWidget->SetEnabled(0);
		this->SelectionZWidget->SetEnabled(0);
	}
}

//------------------------------------------------------------------------------
void vtkTransformWidget::SelectAction(vtkAbstractWidget* w)
{
	vtkTransformWidget* self = vtkTransformWidget::SafeDownCast(w);
	if (!self)
	{
		return;
	}
	if (self->WidgetRep->GetInteractionState() == vtkTransformRepresentation::Outside)
	{
		return;
	}

	// Get the event position
	int X = self->Interactor->GetEventPosition()[0];
	int Y = self->Interactor->GetEventPosition()[1];

	// Make sure we have a renderer to map display -> world
	vtkRenderer* ren = self->CurrentRenderer;
	if (!ren && self->Interactor)
	{
		ren = self->Interactor->FindPokedRenderer(X, Y);
	}

	vtkTransformRepresentation* rep =
		reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep);
	if (ren && rep)
	{
		bool shift = (self->Interactor->GetShiftKey() != 0);

		// Determine what part was clicked. Use the representation's compute method
		// to get a reliable interaction state for the click location.
		int state = rep->ComputeInteractionState(X, Y);

		if (shift)
		{
			// Default "original" origin
			double defaultOrigin[3] = { 0.0, 0.0, 0.0 };

			// 1) Shift + click on origin: reset origin position to defaultOrigin,
			//    but preserve current rotation. Then update selection tips to follow DynamicAxes.
			if (state == vtkTransformRepresentation::OnOrigin)
			{
				// Update origin representation & static transform (SetOriginWorldPosition handles StaticTransform)
				rep->SetOriginWorldPosition(defaultOrigin);

				// Preserve rotation of dynamic transform but set its translation to defaultOrigin.
				vtkTransform* dynT = rep->GetTransform();
				if (dynT)
				{
					vtkMatrix4x4* M = dynT->GetMatrix();
					if (M)
					{
						vtkSmartPointer<vtkMatrix4x4> newM = vtkSmartPointer<vtkMatrix4x4>::New();
						newM->DeepCopy(M);
						// set translation column to defaultOrigin
						newM->SetElement(0, 3, defaultOrigin[0]);
						newM->SetElement(1, 3, defaultOrigin[1]);
						newM->SetElement(2, 3, defaultOrigin[2]);
						dynT->SetMatrix(newM);
					}
					else
					{
						// Fallback: post-translate to default origin (try to preserve rotation if any)
						dynT->PostMultiply();
						dynT->Translate(defaultOrigin);
					}
				}

				// Ensure selection tips follow DynamicAxes ends
				rep->BuildRepresentation();

				// Notify observers that a reset occurred.
				self->Reset();

				// Also notify ValueChanged observers (final state)
				self->InvokeEvent(vtkTransformWidget::ValueChangedEvent, nullptr);

				self->Render();
				if (self->EventCallbackCommand)
				{
					self->EventCallbackCommand->SetAbortFlag(1);
				}
				return;
			}
			// 2) Shift + click on any axis tip: reset rotation only, keep origin translation unchanged.
			else if (state == vtkTransformRepresentation::OnX ||
					 state == vtkTransformRepresentation::OnY ||
					 state == vtkTransformRepresentation::OnZ)
			{
				// Reset rotation while preserving translation of dynamic transform
				vtkTransform* dynT = rep->GetTransform();
				if (dynT)
				{
					vtkMatrix4x4* M = dynT->GetMatrix();
					if (M)
					{
						// Capture current translation
						double tx = M->GetElement(0, 3);
						double ty = M->GetElement(1, 3);
						double tz = M->GetElement(2, 3);

						vtkSmartPointer<vtkMatrix4x4> newM = vtkSmartPointer<vtkMatrix4x4>::New();
						newM->Identity();
						newM->SetElement(0, 3, tx);
						newM->SetElement(1, 3, ty);
						newM->SetElement(2, 3, tz);

						dynT->SetMatrix(newM);
					}
					else
					{
						// Fallback: identity transform, but preserve position via translation
						dynT->Identity();
						// get current origin world and set translation to it
						double originWorld[3];
						rep->GetOriginWorldPosition(originWorld);
						dynT->Translate(originWorld);
					}
				}

				// Update selection tips to reflect new (rotation-reset) DynamicAxes
				rep->BuildRepresentation();

				// Notify observers that a reset occurred.
				self->Reset();

				// Also notify ValueChanged observers (final state)
				self->InvokeEvent(vtkTransformWidget::ValueChangedEvent, nullptr);

				self->Render();
				if (self->EventCallbackCommand)
				{
					self->EventCallbackCommand->SetAbortFlag(1);
				}
				return;
			}
			// If shift but click elsewhere, fall through to normal selection below.
		}

		// If not shift-handling or shift on non-reset area, proceed with usual per-part handling.
		if (state == vtkTransformRepresentation::OnOrigin)
		{
			// Use current origin display Z to map the click (X,Y) into the same plane
			double originWorld[3];
			rep->GetOriginWorldPosition(originWorld);

			// Compute display Z for the origin world
			double dispOrigin[3];
			ren->SetWorldPoint(originWorld[0], originWorld[1], originWorld[2], 1.0);
			ren->WorldToDisplay();
			ren->GetDisplayPoint(dispOrigin);
			double originDisplayZ = dispOrigin[2];

			// Convert click (X,Y, originDisplayZ) -> world
			double displayPt[3] = { static_cast<double>(X), static_cast<double>(Y), originDisplayZ };
			ren->SetDisplayPoint(displayPt);
			ren->DisplayToWorld();
			double* world4 = ren->GetWorldPoint();
			double clickWorld[3] = { 0.0, 0.0, 0.0 };
			if (world4[3] != 0.0)
			{
				clickWorld[0] = world4[0] / world4[3];
				clickWorld[1] = world4[1] / world4[3];
				clickWorld[2] = world4[2] / world4[3];
			}

			// Set origin handle to the clicked world position (this updates StaticTransform
			// inside SetOriginWorldPosition to place static axes correctly)
			rep->SetOriginWorldPosition(clickWorld);

			// Adjust dynamic transform translation so dynamic axes' origin coincides
			// with clickWorld while preserving orientation.
			vtkTransform* dynT = rep->GetTransform();
			if (dynT)
			{
				// Get current dynamic origin in world (Transform applied to local origin)
				double localOrigin[3] = { 0.0, 0.0, 0.0 };
				double dynOriginWorld[3] = { 0.0, 0.0, 0.0 };
				dynT->TransformPoint(localOrigin, dynOriginWorld);

				// Delta needed to move dynamic origin to clickWorld
				double delta[3] = {
					clickWorld[0] - dynOriginWorld[0],
					clickWorld[1] - dynOriginWorld[1],
					clickWorld[2] - dynOriginWorld[2]
				};

				// Apply delta as a post-multiply translation to preserve existing rotation
				dynT->PostMultiply();
				dynT->Translate(delta);
			}
		}
		// else: clicked an axis (OnX/OnY/OnZ) - do not move origin
	}

	// We are definitely selected
	self->WidgetState = vtkTransformWidget::Active;
	self->GrabFocus(self->EventCallbackCommand);
	double e[2];
	e[0] = static_cast<double>(X);
	e[1] = static_cast<double>(Y);
	reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->StartWidgetInteraction(e);
	self->InvokeEvent(vtkCommand::LeftButtonPressEvent, nullptr); // for the handles
	self->StartInteraction();
	self->InvokeEvent(vtkCommand::StartInteractionEvent, nullptr);
	self->EventCallbackCommand->SetAbortFlag(1);
}

//------------------------------------------------------------------------------
// MoveAction remains unchanged
void vtkTransformWidget::MoveAction(vtkAbstractWidget* w)
{
	vtkTransformWidget* self = vtkTransformWidget::SafeDownCast(w);
	if (!self)
	{
		return;
	}
	// compute some info we need for all cases
	int X = self->Interactor->GetEventPosition()[0];
	int Y = self->Interactor->GetEventPosition()[1];

	// See whether we're active
	if (self->WidgetState == vtkTransformWidget::Start)
	{
		self->Interactor->Disable(); // avoid extra renders
		self->OriginWidget->SetEnabled(0);
		self->SelectionXWidget->SetEnabled(0);
		self->SelectionYWidget->SetEnabled(0);
		self->SelectionZWidget->SetEnabled(0);

		int oldState = self->WidgetRep->GetInteractionState();
		int state = self->WidgetRep->ComputeInteractionState(X, Y);
		int changed;
		// Determine if we are near the end points or the line
		if (state == vtkTransformRepresentation::Outside)
		{
			changed = self->RequestCursorShape(VTK_CURSOR_DEFAULT);
		}
		else // must be near something
		{
			changed = self->RequestCursorShape(VTK_CURSOR_HAND);
			if (state == vtkTransformRepresentation::OnOrigin)
			{
				self->OriginWidget->SetEnabled(1);

				// keep representation state in sync so subsequent events keep the hover state
				reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->SetInteractionState(state);
				self->CurrentHandle = 0; // origin
			}
			else if (state == vtkTransformRepresentation::OnX)
			{
				self->SelectionXWidget->SetEnabled(1);
				changed = 1; // movement along the line always needs render

				reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->SetInteractionState(state);
				self->CurrentHandle = 1; // X
			}
			else if (state == vtkTransformRepresentation::OnY)
			{
				self->SelectionYWidget->SetEnabled(1);
				changed = 1; // movement along the line always needs render

				reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->SetInteractionState(state);
				self->CurrentHandle = 2; // Y
			}
			else if (state == vtkTransformRepresentation::OnZ)
			{
				self->SelectionZWidget->SetEnabled(1);
				changed = 1; // movement along the line always needs render

				reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->SetInteractionState(state);
				self->CurrentHandle = 3; // Z
			}
		}
		self->Interactor->Enable(); // avoid extra renders
		if (changed || oldState != state)
		{
			self->Render();
		}
	}
	else // if ( self->WidgetState == vtkTransformWidget::Active )
	{
		// moving something
		double e[2];
		e[0] = static_cast<double>(X);
		e[1] = static_cast<double>(Y);
		self->InvokeEvent(vtkCommand::MouseMoveEvent, nullptr); // handles observe this
		reinterpret_cast<vtkTransformRepresentation*>(self->WidgetRep)->WidgetInteraction(e);
		self->InvokeEvent(vtkCommand::InteractionEvent, nullptr);
		self->EventCallbackCommand->SetAbortFlag(1);
		self->Render();

		// Throttled, optional lightweight notification for observers that want
		// less frequent updates than InteractionEvent. Observers should call
		// GetLineRepresentation() to query state (origin, yaw/pitch/roll).
		if (self->ReportValueDuringInteraction)
		{
			double nowSec = vtkTimerLog::GetUniversalTime();
			double elapsedMs = 0.0;
			if (self->LastValueEventTimeSec < 0.0)
			{
				// never emitted before: treat as if enough time has passed
				elapsedMs = static_cast<double>(self->ValueChangedIntervalMs);
			}
			else
			{
				elapsedMs = (nowSec - self->LastValueEventTimeSec) * 1000.0;
			}

			if (elapsedMs >= static_cast<double>(self->ValueChangedIntervalMs))
			{
				self->InvokeEvent(vtkTransformWidget::ValueChangedEvent, nullptr);
				// record emission using vtkTimeStamp and wall-clock seconds
				self->LastValueEventTime.Modified();
				self->LastValueEventTimeSec = nowSec;
			}
		}
	}
}

//------------------------------------------------------------------------------
// EndSelectAction: ensure final ValueChangedEvent fires so observers can
// update expensive UI (text, file export, etc.) once per interaction.
void vtkTransformWidget::EndSelectAction(vtkAbstractWidget* w)
{
	vtkTransformWidget* self = vtkTransformWidget::SafeDownCast(w);
	if (!self)
	{
		return;
	}
	if (self->WidgetState == vtkTransformWidget::Start)
	{
		std::cout << "start select action in end select action" << std::endl;
		return;
	}

	// Return state to not active
	self->WidgetState = vtkTransformWidget::Start;
	self->ReleaseFocus();
	self->InvokeEvent(vtkCommand::LeftButtonReleaseEvent, nullptr); // handles observe this
	self->EventCallbackCommand->SetAbortFlag(1);
	self->InvokeEvent(vtkCommand::EndInteractionEvent, nullptr);
	self->Superclass::EndInteraction();

	// Notify observers with a final low-frequency update
	self->InvokeEvent(vtkTransformWidget::ValueChangedEvent, nullptr);

	self->Render();
}

//------------------------------------------------------------------------------
// CreateDefaultRepresentation
void vtkTransformWidget::CreateDefaultRepresentation()
{
	if (!this->WidgetRep)
	{
		this->WidgetRep = vtkTransformRepresentation::New();
	}
}

//------------------------------------------------------------------------------
void vtkTransformWidget::SetProcessEvents(vtkTypeBool pe)
{
	this->Superclass::SetProcessEvents(pe);

	this->OriginWidget->SetProcessEvents(pe);
	this->SelectionXWidget->SetProcessEvents(pe);
	this->SelectionYWidget->SetProcessEvents(pe);
	this->SelectionZWidget->SetProcessEvents(pe);
}

void vtkTransformWidget::SetRotationEnabled(vtkTypeBool val)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetRotationEnabled(val);
	}
}

vtkTypeBool vtkTransformWidget::GetRotationEnabled()
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	return rep ? rep->GetRotationEnabled() : static_cast<vtkTypeBool>(0);
}

void vtkTransformWidget::SetTranslationEnabled(vtkTypeBool val)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetTranslationEnabled(val);
	}
}

vtkTypeBool vtkTransformWidget::GetTranslationEnabled()
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	return rep ? rep->GetTranslationEnabled() : static_cast<vtkTypeBool>(0);
}

//------------------------------------------------------------------------------
void vtkTransformWidget::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
}

// Widget forwarders: call representation methods if available.

void vtkTransformWidget::SetYawDegrees(double yawDeg)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetYawDegrees(yawDeg);
		this->Render();
	}
}

void vtkTransformWidget::SetPitchDegrees(double pitchDeg)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetPitchDegrees(pitchDeg);
		this->Render();
	}
}

void vtkTransformWidget::SetRollDegrees(double rollDeg)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetRollDegrees(rollDeg);
		this->Render();
	}
}

void vtkTransformWidget::SetYawPitchRollDegrees(double yawDeg, double pitchDeg, double rollDeg)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->SetYawPitchRollDegrees(yawDeg, pitchDeg, rollDeg);
		this->Render();
	}
}

void vtkTransformWidget::GetYawPitchRollDegrees(double& yawDeg, double& pitchDeg, double& rollDeg)
{
	vtkTransformRepresentation* rep = static_cast<vtkTransformRepresentation*>(this->WidgetRep);
	if (rep)
	{
		rep->GetYawPitchRollDegrees(yawDeg, pitchDeg, rollDeg);
	}
	else
	{
		yawDeg = pitchDeg = rollDeg = 0.0;
	}
}
