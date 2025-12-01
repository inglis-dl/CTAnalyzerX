/**
 * @class   vtkTransformWidget
 * @brief   3D widget for performing 3D transformations around an axes
 *
 * Summary:
 * The vtkTransformWidget provides interactive controls to translate, rotate,
 * and scale objects using an axes-guided widget. It is a higher-level
 * controller which composes several internal vtkHandleWidget instances and
 * delegates geometry/visualization to a vtkTransformRepresentation (or a
 * subclass). The widget listens to interactor events and maps them to widget
 * events (Select, EndSelect, Move) which are translated into the VTK
 * interaction events (StartInteractionEvent, InteractionEvent, EndInteractionEvent).
 *
 * Responsibilities:
 *  - Create and manage internal handle widgets (origin + three selection widgets).
 *  - Forward input events to the representation for hit-testing and interaction.
 *  - Provide convenience API to forward translation/rotation enabling and
 *    yaw/pitch/roll setters/getters to the representation.
 *  - Emit interaction events that application code can observe.
 *
 * Event model:
 *  - The widget watches interactor events and maps them to:
 *      vtkCommand::StartInteractionEvent  (widget start)
 *      vtkCommand::InteractionEvent       (widget in-motion)
 *      vtkCommand::EndInteractionEvent    (widget end)
 *  - Applications should add observers to the widget to be notified when the
 *    widget changes transform/placement. Observers receive a vtkObject* caller
 *    and can query the attached representation (GetLineRepresentation()).
 *
 * Usage example:
 *  vtkNew<vtkTransformWidget> widget;
 *  widget->SetInteractor(iren);
 *  widget->SetRepresentation(myRep); // myRep is vtkTransformRepresentation*
 *  widget->SetEnabled(1);
 *
 * Notes:
 *  - CreateDefaultRepresentation() will instantiate a vtkTransformRepresentation
 *    if no representation has been set.
 *  - SetEnabled enables the internal handle widgets when the widget becomes active.
 *
 * @sa
 * vtkTransformRepresentation
 */

#ifndef vtkTransformWidget_h
#define vtkTransformWidget_h

#include "vtkAbstractWidget.h"
#include "vtkTimeStamp.h" // used for VTK-native timestamping
 // (we use vtkTimerLog::GetUniversalTime() in implementation to compute elapsed ms)

class vtkTransform;
class vtkTransformRepresentation;
class vtkHandleWidget;

class vtkTransformWidget : public vtkAbstractWidget
{
public:
	/**
	 * Instantiate the object.
	 */
	static vtkTransformWidget* New();

	///@{
	/**
	 * Standard vtkObject methods
	 */
	vtkTypeMacro(vtkTransformWidget, vtkAbstractWidget);
	void PrintSelf(ostream& os, vtkIndent indent) override;
	///@}

	/**
	 * Override superclasses' SetEnabled() method because the widget must
	 * enable/disable its internal handle widgets. When enabling, the widget
	 * will create a default representation if none is set, attach the
	 * representation's handle reps to the internal handle widgets and set the
	 * interactor/renderer appropriately.
	 */
	void SetEnabled(int enabling) override;

	/**
	 * Specify an instance of vtkWidgetRepresentation used to represent this
	 * widget in the scene.
	 */
	void SetRepresentation(vtkTransformRepresentation* r)
	{
		this->Superclass::SetWidgetRepresentation(reinterpret_cast<vtkWidgetRepresentation*>(r));
	}

	vtkTransformRepresentation* GetLineRepresentation()
	{
		return reinterpret_cast<vtkTransformRepresentation*>(this->WidgetRep);
	}

	void CreateDefaultRepresentation() override;
	void SetProcessEvents(vtkTypeBool) override;

	// ---------------------------------------------------------------------
	// Convenience API: forward enable/disable of translation/rotation to the representation
	void SetTranslationEnabled(vtkTypeBool val);
	vtkTypeBool GetTranslationEnabled();

	void TranslationEnabledOn() { this->SetTranslationEnabled(1); }
	void TranslationEnabledOff() { this->SetTranslationEnabled(0); }

	void SetRotationEnabled(vtkTypeBool val);
	vtkTypeBool GetRotationEnabled();

	void RotationEnabledOn() { this->SetRotationEnabled(1); }
	void RotationEnabledOff() { this->SetRotationEnabled(0); }

	// Convenience forwarders so UI code can call the widget directly.
	void SetYawDegrees(double yawDeg);
	void SetPitchDegrees(double pitchDeg);
	void SetRollDegrees(double rollDeg);
	void SetYawPitchRollDegrees(double yawDeg, double pitchDeg, double rollDeg);

	void GetYawPitchRollDegrees(double& yawDeg, double& pitchDeg, double& rollDeg);

	// New: lower-frequency / lightweight state notification event.
	// Observers can listen for this event instead of the high-frequency
	// vtkCommand::InteractionEvent. The widget will emit this event at most
	// every ValueChangedIntervalMs when reporting during continuous interaction.
	static const unsigned long ResetEvent;         // existing: signals a reset action
	static const unsigned long ValueChangedEvent;  // new: low-frequency state update

	// Configure reporting behaviour (defaults set in constructor)
	void SetValueChangedInterval(int ms);               // minimum interval between ValueChangedEvent invocations during interaction
	int GetValueChangedInterval() const;
	void SetReportValueDuringInteraction(vtkTypeBool val); // whether ValueChangedEvent is fired while widget is moving
	vtkTypeBool GetReportValueDuringInteraction() const;

	/**
	 * Reset the widget to its initial state. Emits ResetEvent and notifies observers.
	 */
	void Reset();

protected:
	vtkTransformWidget();
	~vtkTransformWidget() override;

	int WidgetState;
	enum WidgetStateType
	{
		Start = 0,
		Active
	};

	typedef WidgetStateType _WidgetState;
	int CurrentHandle;

	// These methods handle events
	static void SelectAction(vtkAbstractWidget*);
	static void EndSelectAction(vtkAbstractWidget*);
	static void MoveAction(vtkAbstractWidget*);

	vtkTransform* Transform;

	// The positioning handle widgets
	vtkHandleWidget* OriginWidget;    // center end point for translation
	vtkHandleWidget* SelectionXWidget; // select the end of an axis
	vtkHandleWidget* SelectionYWidget; // select the end of an axis
	vtkHandleWidget* SelectionZWidget; // select the end of an axis

	// Performance / reporting configuration
	int ValueChangedIntervalMs; // milliseconds between ValueChangedEvent during interaction
	vtkTypeBool ReportValueDuringInteraction;

	// Timestamp of the last ValueChangedEvent emission (VTK timestamp) and last recorded wall-clock
	vtkTimeStamp LastValueEventTime; // use Modified() when we emit the ValueChangedEvent
	double LastValueEventTimeSec;    // last emission time in seconds from vtkTimerLog::GetUniversalTime()

private:
	vtkTransformWidget(const vtkTransformWidget&) = delete;
	void operator=(const vtkTransformWidget&) = delete;
};

#endif
