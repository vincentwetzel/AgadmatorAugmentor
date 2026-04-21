#pragma once

#include <QDialog>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <vector>
#include <QStringList>

#include "VideoOverlayConfig.h"
#include "TemplateManager.h"

namespace aa {

/**
 * @class DraggableOverlay
 * @brief A custom QGraphicsPixmapItem that constrains its movement to the video bounds
 *        and handles absolute/percentage coordinate conversions.
 */
class DraggableOverlay : public QGraphicsPixmapItem {
public:
    explicit DraggableOverlay(const QPixmap& pixmap, const QString& id, QGraphicsItem* parent = nullptr);
    
    void setVideoBounds(const QSizeF& bounds);
    void updateFromConfig(const OverlayElement& elem);
    void populateConfig(OverlayElement& elem) const;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    enum ResizeHandle { None, TopLeft, TopRight, BottomLeft, BottomRight };
    ResizeHandle getHandleAt(const QPointF& pos) const;

    QString id_;
    QSizeF videoBounds_;
    double currentScale_ = 1.0;
    bool isResizing_ = false;
    ResizeHandle activeHandle_ = None;
};

/**
 * @class OverlayEditorDialog
 * @brief A WYSIWYG editor dialog allowing users to position overlays via drag-and-drop.
 */
class OverlayEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit OverlayEditorDialog(QWidget* parent = nullptr);
    ~OverlayEditorDialog() override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void onTemplateChanged(int index);
    void onNewTemplate();
    void onDeleteTemplate();
    void onChangeScreenshot();
    void onTogglesChanged();
    void onAccept();

private:
    void setupUi();
    void setupOverlays();
    void updateOverlayBounds();
    void loadTemplateToUi(int index);
    void saveUiToTemplate(int index);
    void refreshTemplateCombo();

    std::vector<OverlayTemplate> templates_;
    std::vector<QString> deletedTemplateIds_;
    int currentIndex_ = -1;

    QGraphicsScene* scene_;
    QGraphicsView* view_;
    QGraphicsPixmapItem* backgroundItem_;
    
    DraggableOverlay* boardItem_;
    DraggableOverlay* evalBarItem_;
    DraggableOverlay* pvTextItem_;
    
    QComboBox* templateCombo_;
    QPushButton* newTemplateBtn_;
    QPushButton* deleteTemplateBtn_;
    QPushButton* changeScreenshotBtn_;
    QLineEdit* templateNameEdit_;
    QLineEdit* templateKeywordsEdit_;
    
    QCheckBox* boardCheck_;
    QCheckBox* evalCheck_;
    QCheckBox* pvCheck_;
};

} // namespace aa