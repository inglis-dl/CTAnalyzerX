#pragma once

#include <QFrame>
#include <QColor>

class QHBoxLayout;
class QVBoxLayout;
class QLabel;
class QAction;
class QToolButton;
class QWidget;
class QEvent;
class QKeyEvent;

class MenuButton;

class SelectionFrameWidget : public QFrame
{
	Q_OBJECT
		// Expose core state as properties for Designer/bindings
		Q_PROPERTY(QString title READ getTitle WRITE setTitle NOTIFY titleChanged)
		Q_PROPERTY(bool selected READ isSelected WRITE setSelected NOTIFY selectedChanged)
		Q_PROPERTY(QString currentItem READ currentItem WRITE setCurrentItem NOTIFY currentItemChanged)
		Q_PROPERTY(bool selectionListVisible READ selectionListVisible WRITE setSelectionListVisible)
		Q_PROPERTY(bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible)
		Q_PROPERTY(int outerBorderWidth READ outerBorderWidth WRITE setOuterBorderWidth)
		Q_PROPERTY(bool allowChangeTitle READ allowChangeTitle WRITE setAllowChangeTitle)
		Q_PROPERTY(bool allowClose READ allowClose WRITE setAllowClose)
		// Color customization (split for Designer friendliness)
		Q_PROPERTY(QColor titleForegroundColor READ titleForegroundColor WRITE setTitleForegroundColor)
		Q_PROPERTY(QColor titleBackgroundColor READ titleBackgroundColor WRITE setTitleBackgroundColor)
		Q_PROPERTY(QColor selectedTitleForegroundColor READ selectedTitleForegroundColor WRITE setSelectedTitleForegroundColor)
		Q_PROPERTY(QColor selectedTitleBackgroundColor READ selectedTitleBackgroundColor WRITE setSelectedTitleBackgroundColor)
		Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor)
		Q_PROPERTY(QColor borderSelectedColor READ borderSelectedColor WRITE setBorderSelectedColor)
		Q_PROPERTY(bool restrictInteractionToSelection READ restrictInteractionToSelection WRITE setRestrictInteractionToSelection)

public:
	explicit SelectionFrameWidget(QWidget* parent = nullptr);

	void setTitle(const QString& title);
	QString getTitle() const;

	// Populate the selection list (separators via "--").
	void setSelectionList(const QStringList& items);

	// Programmatic selection
	void setCurrentItem(const QString& item);
	QString currentItem() const;

	// Body content
	void setCentralWidget(QWidget* widget);
	QWidget* centralWidget() const;

	// Expose the menu button if consumers need direct access
	MenuButton* menuButton() const;

	// Selected state and visuals
	void setSelected(bool selected);
	bool isSelected() const { return m_selected; }

	void setSelectionListVisible(bool visible);
	bool selectionListVisible() const { return m_selectionListVisible; }

	void setTitleBarVisible(bool visible);
	bool titleBarVisible() const { return m_titleBarVisible; }

	void setOuterBorderWidth(int px);
	int outerBorderWidth() const { return m_outerBorderWidth; }

	// Batch setters kept for convenience
	void setTitleColors(const QColor& foreground, const QColor& background);
	void setSelectedTitleColors(const QColor& foreground, const QColor& background);
	void setBorderColors(const QColor& normal, const QColor& selected);

	// Property-friendly individual color accessors
	QColor titleForegroundColor() const { return m_titleFg; }
	void setTitleForegroundColor(const QColor& c);

	QColor titleBackgroundColor() const { return m_titleBg; }
	void setTitleBackgroundColor(const QColor& c);

	QColor selectedTitleForegroundColor() const { return m_selectedTitleFg; }
	void setSelectedTitleForegroundColor(const QColor& c);

	QColor selectedTitleBackgroundColor() const { return m_selectedTitleBg; }
	void setSelectedTitleBackgroundColor(const QColor& c);

	QColor borderColor() const { return m_borderColor; }
	void setBorderColor(const QColor& c);

	QColor borderSelectedColor() const { return m_borderSelectedColor; }
	void setBorderSelectedColor(const QColor& c);

	void setAllowChangeTitle(bool on);
	bool allowChangeTitle() const { return m_allowChangeTitle; }

	void setAllowClose(bool on);
	bool allowClose() const { return m_allowClose; }

	void setRestrictInteractionToSelection(bool on);
	bool restrictInteractionToSelection() const { return m_restrictInteractionToSelection; }

	// Add small actions placed on the right side of the header
	QAction* addHeaderAction(QAction* action);

signals:
	void selectionChanged(const QString& item);
	void currentItemChanged(const QString& item);
	void titleChanged(const QString& title);
	void selectedChanged(bool selected);
	void doubleClicked();
	void requestClose(); // For layout manager use

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
	void keyPressEvent(QKeyEvent* e) override;
	void resizeEvent(QResizeEvent* e) override;
	void showEvent(QShowEvent* e) override;
	void focusInEvent(QFocusEvent* e) override;

	// Notifies derived widgets (e.g., SliceView/VolumeView) so they can enable/disable VTK interactor
	virtual void onSelectionChanged(bool selected) {}

private:
	void updateVisuals();
	void appendAuxMenuActions(); // Append "Change Title..." / "Close" to the menu if enabled
	void syncMenuCheckedFromTitle(); // Make the checked action follow the title
	void beginEditTitle(); // F2 / menu handler
	void syncMenuButtonSizeToHeader();

private:
	// Header container to catch clicks/double-clicks
	QWidget* m_headerContainer;
	QHBoxLayout* m_headerLayout;
	QVBoxLayout* m_mainLayout;
	MenuButton* m_selectionMenuButton;
	QLabel* m_titleLabel;
	QWidget* m_centralWidget;

	// Right-aligned header actions area
	QWidget* m_headerActionsContainer;
	QHBoxLayout* m_headerActionsLayout;

	// State
	bool m_selected = false;
	bool m_selectionListVisible = true;
	bool m_titleBarVisible = true;
	bool m_allowChangeTitle = true;
	bool m_allowClose = true;

	int m_outerBorderWidth = 0;

	QColor m_titleFg;
	QColor m_titleBg;
	QColor m_selectedTitleFg;
	QColor m_selectedTitleBg;
	QColor m_borderColor;
	QColor m_borderSelectedColor;

	bool m_restrictInteractionToSelection = true; // default: single-frame focus
};