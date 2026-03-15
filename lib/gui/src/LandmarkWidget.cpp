#include "LandmarkWidget.h"
#include "ui_LandmarkWidget.h"
#include "LightboxWidget.h"
#include "SliceView.h"
#include "VolumeView.h"
#include "LandmarkHelper.h"

#include <QPushButton>
#include <QSlider>
#include <QLineEdit>
#include <QLabel>
#include <QIntValidator>
#include <QJsonObject>

#include <vtkSmartPointer.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkVertexGlyphFilter.h>

LandmarkWidget::LandmarkWidget(QWidget* parent)
	: QWidget(parent)
	, ui(new Ui::LandmarkWidget)
	, m_fids(3)
{
	ui->setupUi(this);
	init();
	m_fidPolyData = vtkSmartPointer<vtkPolyData>::New();
	updateControlStates();
}

LandmarkWidget::~LandmarkWidget()
{
	delete ui;
}

void LandmarkWidget::init()
{
	// Assign UI controls to member pointers for convenience
	m_btnDefine = ui->m_btnDefine;
	m_btnSave = ui->m_btnSave;
	m_btnDelete = ui->m_btnDelete;
	m_btnReset = ui->m_btnReset;

	m_sliders[0] = ui->m_slider0;
	m_sliders[1] = ui->m_slider1;
	m_sliders[2] = ui->m_slider2;

	m_edits[0] = ui->m_edit0;
	m_edits[1] = ui->m_edit1;
	m_edits[2] = ui->m_edit2;

	m_lblStatus = ui->m_lblStatus;

	// Use integer validators for index-based input
	for (int i = 0; i < 3; ++i) {
		m_edits[i]->setValidator(new QIntValidator(0, 0, m_edits[i]));
	}

	// Connect buttons
	connect(m_btnDefine, &QPushButton::toggled, this, &LandmarkWidget::onDefineToggled);
	connect(m_btnSave, &QPushButton::clicked, this, &LandmarkWidget::onSaveClicked);
	connect(m_btnDelete, &QPushButton::clicked, this, &LandmarkWidget::onDeleteClicked);
	connect(m_btnReset, &QPushButton::clicked, this, &LandmarkWidget::onResetClicked);

	// Connect sliders/edits (working in index space)
	for (int i = 0; i < 3; ++i) {
		// Slider -> edit (direct index value)
		connect(m_sliders[i], &QSlider::valueChanged, [this, i](int indexVal) {
			m_edits[i]->setText(QString::number(indexVal));
		});

		// Edit -> slider
		connect(m_edits[i], &QLineEdit::editingFinished, [this, i]() {
			bool ok = false;
			int indexVal = m_edits[i]->text().toInt(&ok);
			if (ok) {
				m_sliders[i]->setValue(indexVal);
			}
		});
	}

	m_lblStatus->setText(tr("0 / 3 landmarks defined"));
}

void LandmarkWidget::setLightbox(LightboxWidget* lightbox)
{
	if (m_lightbox == lightbox) return;

	m_lightbox = lightbox;

	if (!m_lightbox) return;

	// Create coordinator if not already exists
	if (!m_helper) {
		m_helper = new LandmarkHelper(this);
		m_helper->setMaxLandmarks(3);

		// Connect coordinator signals to LandmarkWidget handlers
		connect(m_helper, &LandmarkHelper::landmarkPlaced,
				this, &LandmarkWidget::handleLandmarkPlaced);
		connect(m_helper, &LandmarkHelper::landmarkMoved,
				this, &LandmarkWidget::handleLandmarkMoved);
		connect(m_helper, &LandmarkHelper::landmarkDeleted,
				this, &LandmarkWidget::handleLandmarkDeleted);
	}

	// Register all views with coordinator
	if (auto* xy = m_lightbox->getXYView()) {
		m_helper->registerView(xy);
	}

	if (auto* xz = m_lightbox->getXZView()) {
		m_helper->registerView(xz);
	}

	if (auto* yz = m_lightbox->getYZView()) {
		m_helper->registerView(yz);
	}

	// Setup view selection/focus connections
	setupViewConnections();
}

void LandmarkWidget::setupViewConnections()
{
	if (!m_lightbox || !m_helper) return;

	auto connectViewSelection = [this](SliceView* view) {
		if (!view) return;

		// Connect to SelectionFrameWidget::selectedChanged signal
		// (No need to cast - SliceView IS-A SelectionFrameWidget via ImageFrameWidget)
		connect(view, &SelectionFrameWidget::selectedChanged,
				this, [this](bool selected) { // Don't capture 'view'
					auto* view = qobject_cast<SliceView*>(sender()); // Get from signal
					if (!view || !m_helper || !m_helper->isEnabled()) return;

					if (selected) {
						// View selected -> activate coordinator on it
						m_helper->setActiveView(view);
					}
					else {
						// View deselected -> if it was active, clear or switch
						if (m_helper->activeView() == view) {
							// Find another selected view or detach
							SliceView* newActive = nullptr;
							for (auto* v : { m_lightbox->getXYView(),
											m_lightbox->getXZView(),
											m_lightbox->getYZView() }) {
								if (v && v != view && v->isSelected()) {
									newActive = v;
									break;
								}
							}

							if (newActive) {
								m_helper->setActiveView(newActive);
							}
							else {
								// No selected view -> detach widget
								m_helper->setActiveView(nullptr);
							}
						}
					}
		}, Qt::UniqueConnection);

		// Also handle focus events (optional)
		view->installEventFilter(this);
		};

	connectViewSelection(m_lightbox->getXYView());
	connectViewSelection(m_lightbox->getXZView());
	connectViewSelection(m_lightbox->getYZView());

	// Set initial active view
	if (auto* xy = m_lightbox->getXYView()) {
		if (xy->isSelected()) {
			m_helper->setActiveView(xy);
		}
	}
}

bool LandmarkWidget::eventFilter(QObject* watched, QEvent* event)
{
	// Detect when a SliceView gains focus and activate coordinator on it
	if (event->type() == QEvent::FocusIn) {
		if (auto* view = qobject_cast<SliceView*>(watched)) {
			if (m_helper) {
				m_helper->setActiveView(view);
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}

void LandmarkWidget::updateControlStates()
{
	if (!m_helper) return;

	// Count defined landmarks
	int definedCount = 0;
	for (const auto& f : m_fids) {
		if (f.defined) ++definedCount;
	}

	// Add button: enabled only if 3 defined
	if (m_btnSave) {
		m_btnSave->setEnabled(definedCount == 3);
	}

	// Delete button: enabled only if there's an active landmark in coordinator
	bool hasActive = false;
	if (m_helper && m_helper->isEnabled()) {
		// Check if coordinator has an active handle
		hasActive = (definedCount > 0);
	}

	if (m_btnDelete) {
		m_btnDelete->setEnabled(hasActive && definedCount > 0);
	}

	// Reset button: enabled only if at least one defined
	if (m_btnReset) {
		m_btnReset->setEnabled(definedCount > 0);
	}

	// Sliders/edits: enabled only when Define mode is active
	bool enableEditing = m_btnDefine && m_btnDefine->isChecked();
	for (int i = 0; i < 3; ++i) {
		if (m_sliders[i]) m_sliders[i]->setEnabled(enableEditing);
		if (m_edits[i]) m_edits[i]->setEnabled(enableEditing);
	}

	// Update status label
	if (m_lblStatus) {
		m_lblStatus->setText(QString("%1 / 3 landmarks defined").arg(definedCount));
	}
}

QJsonArray LandmarkWidget::currentLandmarksAsJson() const
{
	QJsonArray out;
	for (const auto& f : m_fids) {
		QJsonObject o;
		o["defined"] = f.defined;
		if (f.defined) {
			o["x"] = f.x;
			o["y"] = f.y;
			o["z"] = f.z;
			o["label"] = f.label;
			o["color"] = f.color.name();
		}
		out.append(o);
	}
	return out;
}

void LandmarkWidget::loadLandmarksFromJson(const QJsonArray& arr)
{
	if (!m_helper) return;

	// Reset current state
	m_helper->reset();
	for (auto& f : m_fids) f.defined = false;

	// Load from JSON
	for (int i = 0; i < 3 && i < arr.size(); ++i) {
		QJsonObject o = arr[i].toObject();
		if (o.value("defined").toBool(false)) {
			m_fids[i].defined = true;
			m_fids[i].x = o.value("x").toDouble();
			m_fids[i].y = o.value("y").toDouble();
			m_fids[i].z = o.value("z").toDouble();
			m_fids[i].label = o.value("label").toString();
			m_fids[i].color = QColor(o.value("color").toString());

			// Set in coordinator
			m_helper->setLandmarkWorldPosition(i, m_fids[i].x, m_fids[i].y, m_fids[i].z);
		}
	}

	updateUiFromCurrent();
	updateVolumeRepresentation();
	emitState();
	updateControlStates();
}

void LandmarkWidget::onDefineToggled(bool on)
{
	if (m_helper) {
		m_helper->setEnabled(on);
	}
	updateControlStates();
}

void LandmarkWidget::onSaveClicked()
{
	if (!m_helper) {
		qWarning() << "Cannot save landmarks: no helper available";
		return;
	}

	// Verify all 3 landmarks are defined
	int definedCount = 0;
	for (const auto& fid : m_fids) {
		if (fid.defined) {
			definedCount++;
		}
	}

	if (definedCount != 3) {
		qWarning() << "Cannot save: not all landmarks are defined (" << definedCount << "/3)";
		return;
	}

	// Get current landmarks as JSON
	QJsonArray landmarks = currentLandmarksAsJson();

	if (landmarks.isEmpty() || landmarks.size() != 3) {
		qWarning() << "Invalid landmarks array size:" << landmarks.size();
		return;
	}

	// Emit request signal - MainWindow/StateMachine will handle persistence
	emit saveLandmarksRequested(landmarks);

	qDebug() << "User requested save of 3 valid landmarks";
}

void LandmarkWidget::onDeleteClicked()
{
	if (!m_helper) return;

	// Find last defined landmark and delete it
	int lastDefined = -1;
	for (int i = 2; i >= 0; --i) {
		if (m_fids[i].defined) {
			lastDefined = i;
			break;
		}
	}

	if (lastDefined >= 0) {
		m_fids[lastDefined].defined = false;

		// Delete from coordinator (it will emit landmarkDeleted signal)
		// For now, we manually manage the model and tell coordinator
		// In a more sophisticated setup, coordinator would handle deletion internally

		// Since vtkSeedWidget doesn't have a direct "delete specific index" that's safe,
		// we reset and rebuild
		m_helper->reset();
		for (int i = 0; i < 3; ++i) {
			if (m_fids[i].defined) {
				m_helper->setLandmarkWorldPosition(i, m_fids[i].x, m_fids[i].y, m_fids[i].z);
			}
		}

		updateUiFromCurrent();
		updateVolumeRepresentation();
		emitState();
		updateControlStates();
	}
}

void LandmarkWidget::onResetClicked()
{
	if (!m_helper) return;

	m_helper->reset();
	for (auto& f : m_fids) f.defined = false;

	updateUiFromCurrent();
	updateVolumeRepresentation();
	emitState();
	updateControlStates();
}

void LandmarkWidget::updateUiFromCurrent()
{
	if (!m_lightbox) return;

	SliceView* view = m_lightbox->getXYView();
	if (!view) view = m_lightbox->getXZView();
	if (!view) view = m_lightbox->getYZView();
	if (!view) return;

	auto* image = view->imageData();
	if (!image) return;

	// Show the most recently defined landmark in UI controls
	for (int i = 2; i >= 0; --i) {
		if (m_fids[i].defined) {
			// Convert world -> continuous index
			double contIdx[3];
			image->TransformPhysicalPointToContinuousIndex(m_fids[i].x, m_fids[i].y, m_fids[i].z, contIdx);

			// Round to nearest integer index
			int indices[3] = {
				static_cast<int>(std::round(contIdx[0])),
				static_cast<int>(std::round(contIdx[1])),
				static_cast<int>(std::round(contIdx[2]))
			};

			// Update UI with index values
			for (int axis = 0; axis < 3; ++axis) {
				m_edits[axis]->setText(QString::number(indices[axis]));
				m_sliders[axis]->setValue(indices[axis]);
			}
			break; // Show most recent
		}
	}
}

void LandmarkWidget::updateVolumeRepresentation()
{
	auto pts = vtkSmartPointer<vtkPoints>::New();
	for (int i = 0; i < 3; ++i) {
		if (m_fids[i].defined) {
			pts->InsertNextPoint(m_fids[i].x, m_fids[i].y, m_fids[i].z);
		}
	}
	m_fidPolyData->SetPoints(pts);

	// TODO: Forward to VolumeView when setLandmarkPolyData() is implemented
	/*
	if (m_lightbox) {
		VolumeView* vv = m_lightbox->getVolumeView();
		if (vv) vv->setLandmarkPolyData(m_fidPolyData);
	}
	*/
}

void LandmarkWidget::emitState()
{
	QJsonArray arr = currentLandmarksAsJson();
	emit landmarksChanged(arr);

	bool complete = true;
	for (const auto& f : m_fids) {
		if (!f.defined) {
			complete = false;
			break;
		}
	}
	emit placingComplete(complete);
}

void LandmarkWidget::updateExtents(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	// Configure sliders/validators with image index extents
	m_sliders[0]->setRange(xMin, xMax);
	m_sliders[1]->setRange(yMin, yMax);
	m_sliders[2]->setRange(zMin, zMax);

	// Update validators
	if (auto* v0 = qobject_cast<QIntValidator*>(const_cast<QValidator*>(m_edits[0]->validator()))) {
		v0->setRange(xMin, xMax);
	}
	if (auto* v1 = qobject_cast<QIntValidator*>(const_cast<QValidator*>(m_edits[1]->validator()))) {
		v1->setRange(yMin, yMax);
	}
	if (auto* v2 = qobject_cast<QIntValidator*>(const_cast<QValidator*>(m_edits[2]->validator()))) {
		v2->setRange(zMin, zMax);
	}

	updateControlStates();
}

// ===== Coordinator Signal Handlers =====

void LandmarkWidget::handleLandmarkPlaced(int index, double x, double y, double z)
{
	if (index < 0 || index >= 3) return;

	// Update local model
	m_fids[index].defined = true;
	m_fids[index].x = x;
	m_fids[index].y = y;
	m_fids[index].z = z;

	if (m_fids[index].label.isEmpty()) {
		m_fids[index].label = QString("Fid %1").arg(index + 1);
	}

	updateUiFromCurrent();
	updateVolumeRepresentation();
	emitState();
	updateControlStates();
}

void LandmarkWidget::handleLandmarkMoved(int index, double x, double y, double z)
{
	if (index < 0 || index >= 3) return;

	// Update local model
	m_fids[index].x = x;
	m_fids[index].y = y;
	m_fids[index].z = z;

	updateUiFromCurrent();
	updateVolumeRepresentation();
	emitState();
}

void LandmarkWidget::handleLandmarkDeleted(int remainingCount)
{
	// Mark landmarks beyond remainingCount as undefined
	for (int i = remainingCount; i < 3; ++i) {
		m_fids[i].defined = false;
	}

	updateUiFromCurrent();
	updateVolumeRepresentation();
	emitState();
	updateControlStates();
}

void LandmarkWidget::handleViewSelected(SliceView* view)
{
	if (m_helper && view) {
		m_helper->setActiveView(view);
	}
}