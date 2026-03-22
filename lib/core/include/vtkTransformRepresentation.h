/**
 * @class   vtkTransformRepresentation
 * @brief   represent the vtkTransformWidget
 *
 * The vtkTransformRepresentation is a representation for the
 * vtkTransformWidget. This representation consists of an origin sphere
 * with three tubed axes with cones at the end of the axes. In addition an
 * optional label provides delta values of motion. Note that this particular
 * widget draws its representation in 3D space, so the widget can be
 * occluded.
 *
 * Detailed description:
 * - Visuals:
 *   - An origin handle (sphere) representing the widget origin.
 *   - Three axes (X/Y/Z) generated from vtkAxes sources. The class keeps two
 *     axis sets:
 *     - StaticAxes: translated to follow origin only (no rotation) - useful
 *       to represent a translation-only reference frame.
 *     - DynamicAxes: full transform (rotation + translation) applied so the
 *       axes represent the current orientation of the transform.
 *   - Selection tip handles (small sphere handle reps) placed at axis ends
 *     used for selecting axes for rotation/scale operations.
 *
 * - Transforms:
 *   - Transform: full transform applied to DynamicAxes (rotation + translation)
 *   - StaticTransform: translation-only transform applied to StaticAxes
 *
 * - Interaction and usage:
 *   - The widget/representation support translation (moving the origin) and
 *     rotation (around the three axes). Translation and rotation can be
 *     enabled/disabled independently via the TranslationEnabled and
 *     RotationEnabled flags.
 *   - Interaction state enum (Outside, OnOrigin, OnX, OnY, OnZ) is used for
 *     hit-testing and to drive widget behavior.
 *   - The representation exposes APIs to get/set origin in both world and
 *     display coordinates, to query/set yaw/pitch/roll in degrees, and to
 *     preserve/align selection tip handles.
 *
 * - Yaw/Pitch/Roll convention:
 *   - Yaw  = rotation about +Z
 *   - Pitch = rotation about +Y
 *   - Roll = rotation about +X
 *   - Rotation order (intrinsic) used by getters/setters: Z (yaw) then Y (pitch)
 *     then X (roll) (aka intrinsic Z-Y-X).
 *
 * - Typical pairing:
 *   - Create a vtkTransformWidget, create or obtain a vtkTransformRepresentation,
 *     assign the representation to the widget (SetRepresentation) and ensure the
 *     representation is placed and initialized (PlaceWidget / SetOriginWorldPosition).
 *
 * - Example (conceptual):
 *   vtkNew<vtkTransformRepresentation> rep;
 *   rep->PlaceWidget(bounds);
 *   rep->SetOriginWorldPosition(center);
 *   rep->SetYawPitchRollDegrees(30.0, 0.0, 0.0);
 *
 * @sa
 * vtkTransformWidget vtkDistanceWidget vtkDistanceRepresentation vtkDistanceRepresentation2D
 */

#ifndef vtkTransformRepresentation_h
#define vtkTransformRepresentation_h

#include <vtkWidgetRepresentation.h>

class vtkHandleRepresentation;
class vtkPoints;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkActor;
class vtkAxes;
class vtkBox;
class vtkGlyph3D;
class vtkDoubleArray;
class vtkTransformPolyDataFilter;
class vtkTransform;
class vtkProperty;
class vtkSphereHandleRepresentation;

class vtkTransformRepresentation : public vtkWidgetRepresentation
{
public:
	/**
	 * Instantiate class.
	 */
	static vtkTransformRepresentation* New();

	///@{
	/**
	 * Standard VTK methods.
	 *
	 * PrintSelf prints object information useful for debugging. It will
	 * include the current state of primary transforms (Transform and StaticTransform),
	 * the origin display/world positions and the interaction flags.
	 */
	vtkTypeMacro(vtkTransformRepresentation, vtkWidgetRepresentation);
	void PrintSelf(ostream& os, vtkIndent indent) override;
	///@}

	///@{
	/**
	 * Accessors for the handle representations used by the widget.
	 *
	 * The application may further customize appearance by getting these
	 * representations and changing their properties (color, radius, etc.).
	 */
	vtkGetObjectMacro(OriginRepresentation, vtkSphereHandleRepresentation);
	vtkGetObjectMacro(SelectionXRepresentation, vtkSphereHandleRepresentation);
	vtkGetObjectMacro(SelectionYRepresentation, vtkSphereHandleRepresentation);
	vtkGetObjectMacro(SelectionZRepresentation, vtkSphereHandleRepresentation);
	///@}

	///@{
	/**
	 * Methods to Set/Get the widget origin. Both world and display coordinate
	 * variants are provided. When SetOriginWorldPosition() is used the
	 * representation typically updates the StaticTransform so that StaticAxes
	 * track the new origin.
	 *
	 * - GetOriginWorldPosition() returns the current origin in world coordinates.
	 * - SetOriginWorldPosition(pos) moves the origin handle and updates internal
	 *   translation-only transforms used by StaticAxes.
	 * - Set/GetOriginDisplayPosition operate in display (screen) coordinates.
	 */
	double* GetOriginWorldPosition();
	void GetOriginWorldPosition(double pos[3]);
	void SetOriginWorldPosition(double pos[3]);
	void SetOriginDisplayPosition(double pos[3]);
	void GetOriginDisplayPosition(double pos[3]);
	///@}

	/**
	 * Access the transforms used by this representation.
	 * - Transform: full transform applied to DynamicAxes (rotation + translation).
	 *   The Transform's origin is typically the dynamic axes' local origin.
	 * - StaticTransform: translation-only transform applied to StaticAxes.
	 *
	 * Users can query these transforms to read or modify the widget's transforms.
	 * Note: if you modify the Transform directly, keep in mind rotation ordering
	 * assumptions used by the yaw/pitch/roll helpers.
	 */
	vtkGetObjectMacro(Transform, vtkTransform);
	vtkGetObjectMacro(StaticTransform, vtkTransform);

	/**
	 * Specify a scale to control the size of the widget. Large values make the
	 * widget larger. (Placement and mapper scale operations may also affect final size.)
	 */

	 ///@{
	 /**
	  * The tolerance representing the distance to the widget (in pixels) in
	  * which the cursor is considered near enough to the end points of
	  * the widget to be active. A larger tolerance makes it easier to grab handles.
	  */
	vtkSetClampMacro(Tolerance, int, 1, 100);
	vtkGetMacro(Tolerance, int);
	///@}

	/**
	 * Enable/disable translation behavior (when interacting with the origin).
	 * When disabled, origin translation will be ignored during interaction.
	 *
	 * Use TranslationEnabledOff()/On() or SetTranslationEnabled(0/1).
	 */
	vtkSetMacro(TranslationEnabled, vtkTypeBool);
	vtkGetMacro(TranslationEnabled, vtkTypeBool);
	vtkBooleanMacro(TranslationEnabled, vtkTypeBool);

	/**
	 * Enable/disable rotation behavior (when interacting with axis handles).
	 * When disabled, rotation interactions will not apply rotations.
	 */
	vtkSetMacro(RotationEnabled, vtkTypeBool);
	vtkGetMacro(RotationEnabled, vtkTypeBool);
	vtkBooleanMacro(RotationEnabled, vtkTypeBool);


	/**
	 * Enum used to communicate interaction state and hit-testing results.
	 * Values:
	 *   Outside - cursor not near widget
	 *   OnOrigin - cursor near origin handle (translation)
	 *   OnX/OnY/OnZ - cursor near the corresponding axis (rotation/scale)
	 */
	enum
	{
		Outside = 0,
		OnOrigin,
		OnX,
		OnY,
		OnZ
	};

	///@{
	/**
	 * Interaction state accessors. ComputeInteractionState() is used by the
	 * widget to decide how to proceed given a cursor location (display coords).
	 * StartWidgetInteraction/WidgetInteraction drive behavior for active motion.
	 */
	vtkSetClampMacro(InteractionState, int, Outside, OnZ);
	///@}

	///@{
	/**
	 * Methods required by the vtkProp / widget representation API.
	 *
	 * - BuildRepresentation(): build or refresh internal geometry using current
	 *   transforms and placement info.
	 * - ComputeInteractionState(X,Y): return the interaction enum for display
	 *   coordinates (X,Y).
	 * - StartWidgetInteraction(e): prepare the representation for an interaction
	 *   starting at display location e.
	 * - WidgetInteraction(e): update the representation for a move event at e.
	 * - GetBounds(): return bounds of the representation in world coordinates.
	 */
	void BuildRepresentation() override;
	int ComputeInteractionState(int X, int Y, int modify = 0) override;
	void StartWidgetInteraction(double e[2]) override;
	void WidgetInteraction(double e[2]) override;
	double* GetBounds() override;
	///@}

	///@{
	/**
	 * Methods required by vtkProp for rendering lifecyle.
	 *
	 * - ReleaseGraphicsResources(w): release GPU/renderer resources for window w.
	 * - RenderOpaqueGeometry / RenderTranslucentPolygonalGeometry: render passes.
	 */
	void ReleaseGraphicsResources(vtkWindow* w) override;
	int RenderOpaqueGeometry(vtkViewport* viewport) override;
	int RenderTranslucentPolygonalGeometry(vtkViewport* viewport) override;
	///@}

	/**
	 * Save current tip positions and mark them to be preserved on the next BuildRepresentation().
	 * This is a one-shot operation used when the application wants selection-tip handles
	 * to remain in their current world positions even after a subsequent PlaceWidget() / rebuild.
	 */
	void PreserveSelectionHandlesOnce();

	/**
	 * Align the selection-tip handles to the ends of the StaticAxes (world positions).
	 * Public so callers (e.g., the widget on Shift+Click) can force tip handles to follow
	 * the StaticAxes after resetting origin/transforms.
	 */
	void AlignSelectionHandlesToStaticAxes();

	/**
	 * Extract yaw/pitch/roll (degrees) from the DynamicAxes transform.
	 * Convention: Yaw = rotation about Z, Pitch = rotation about Y, Roll = rotation about X.
	 * Rotation order (intrinsic): Z (yaw) * Y (pitch) * X (roll) (aka Z-Y-X).
	 *
	 * Outputs:
	 *   yawDeg, pitchDeg, rollDeg (degrees)
	 */
	void GetYawPitchRollDegrees(double& yawDeg, double& pitchDeg, double& rollDeg);

	/**
	 * Set yaw/pitch/roll (degrees) on the representation's Transform.
	 * These setters preserve the current translation. Rotation ordering
	 * used is Z (yaw) then Y (pitch) then X (roll).
	 *
	 * Inputs:
	 *   yawDeg, pitchDeg, rollDeg (degrees)
	 */
	void SetYawPitchRollDegrees(double yawDeg, double pitchDeg, double rollDeg);
	void SetYawDegrees(double yawDeg);
	void SetPitchDegrees(double pitchDeg);
	void SetRollDegrees(double rollDeg);

	/**
	 * Convenience method to retrieve origin (world) and yaw/pitch/roll (degrees)
	 * in a single call. Fills `out` with:
	 *   out[0..2] = origin.x, origin.y, origin.z
	 *   out[3] = yawDeg, out[4] = pitchDeg, out[5] = rollDeg
	 *
	 * This reduces call overhead for observers that need both pieces of state.
	 */
	void GetOriginAndYPR(double out[6]);

protected:
	vtkTransformRepresentation();
	~vtkTransformRepresentation() override;

	// The handle and the rep used to close the handles
	vtkSphereHandleRepresentation* OriginRepresentation;
	vtkSphereHandleRepresentation* SelectionXRepresentation;
	vtkSphereHandleRepresentation* SelectionYRepresentation;
	vtkSphereHandleRepresentation* SelectionZRepresentation;

	// Selection tolerance for the handles
	int Tolerance;

	// Replace vtkAxesActor with a vtkAxes source + transform filter + mapper + actor
	vtkAxes* StaticAxes;                        // source generating axis polydata (static)
	vtkAxes* DynamicAxes;                       // source generating axis polydata (dynamic)
	vtkTransformPolyDataFilter* StaticAxesFilter;
	vtkTransformPolyDataFilter* DynamicAxesFilter;
	vtkPolyDataMapper* StaticAxesMapper;
	vtkPolyDataMapper* DynamicAxesMapper;
	vtkActor* StaticAxesActor;
	vtkActor* DynamicAxesActor;

	// Transforms to apply to the axes
	vtkTransform* Transform;        // full (rotate + translate) for DynamicAxes
	vtkTransform* StaticTransform;  // translation-only for StaticAxes

	// Support GetBounds() method
	vtkBox* BoundingBox;

	// Enable/disable interaction features
	vtkTypeBool TranslationEnabled;
	vtkTypeBool RotationEnabled;

	double LastEventPosition[3];

	// One-shot preserve flag and saved world positions for selection tips.
	vtkTypeBool PreserveSelectionHandles;
	double SavedSelectionTipX[3];
	double SavedSelectionTipY[3];
	double SavedSelectionTipZ[3];

	/**
	 * Save current world positions of the selection tip handles.
	 * Used internally by PreserveSelectionHandlesOnce().
	 */
	void SaveSelectionHandleWorldPositions();

private:
	vtkTransformRepresentation(const vtkTransformRepresentation&) = delete;
	void operator=(const vtkTransformRepresentation&) = delete;
};

#endif
