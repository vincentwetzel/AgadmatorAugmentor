#include "OverlayEditorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QStandardPaths>
#include <QPainter>
#include <QUuid>
#include <QDateTime>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <algorithm>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QStyleOptionGraphicsItem>
#include <QResizeEvent>
#include <QSettings>
#include <QStringList>

namespace cta {

// ─── DraggableOverlay Implementation ─────────────────────────────────────────

DraggableOverlay::DraggableOverlay(const QPixmap& pixmap, const QString& id, QGraphicsItem* parent)
    : QGraphicsPixmapItem(pixmap, parent), id_(id) {
    setFlags(QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
}

DraggableOverlay::ResizeHandle DraggableOverlay::getHandleAt(const QPointF& pos) const {
    qreal sx = (id_ == "EvalBar") ? transform().m11() : currentScale_;
    qreal sy = (id_ == "EvalBar") ? transform().m22() : currentScale_;
    if (sx == 0.0) sx = 1.0;
    if (sy == 0.0) sy = 1.0;

    qreal hsX = 20.0 / sx;
    qreal hsY = 20.0 / sy;
    hsX = std::min({hsX, boundingRect().width() / 3.0});
    hsY = std::min({hsY, boundingRect().height() / 3.0});
    QRectF r = boundingRect();
    
    qreal midX = r.center().x() - hsX / 2.0;
    qreal midY = r.center().y() - hsY / 2.0;

    if (QRectF(r.left(), r.top(), hsX, hsY).contains(pos)) return TopLeft;
    if (QRectF(r.right() - hsX, r.top(), hsX, hsY).contains(pos)) return TopRight;
    if (QRectF(r.left(), r.bottom() - hsY, hsX, hsY).contains(pos)) return BottomLeft;
    if (QRectF(r.right() - hsX, r.bottom() - hsY, hsX, hsY).contains(pos)) return BottomRight;
    
    if (QRectF(midX, r.top(), hsX, hsY).contains(pos)) return Top;
    if (QRectF(midX, r.bottom() - hsY, hsX, hsY).contains(pos)) return Bottom;
    if (QRectF(r.left(), midY, hsX, hsY).contains(pos)) return Left;
    if (QRectF(r.right() - hsX, midY, hsX, hsY).contains(pos)) return Right;

    return None;
}

void DraggableOverlay::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    ResizeHandle h = getHandleAt(event->pos());
    if (h == TopLeft || h == BottomRight) {
        setCursor(Qt::SizeFDiagCursor);
    } else if (h == TopRight || h == BottomLeft) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (h == Top || h == Bottom) {
        setCursor(Qt::SizeVerCursor);
    } else if (h == Left || h == Right) {
        setCursor(Qt::SizeHorCursor);
    } else {
        setCursor(Qt::OpenHandCursor);
    }
    QGraphicsPixmapItem::hoverMoveEvent(event);
}

void DraggableOverlay::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    activeHandle_ = getHandleAt(event->pos());
    if (activeHandle_ != None) {
        isResizing_ = true;
        setSelected(true);
        event->accept();
    } else {
        QGraphicsPixmapItem::mousePressEvent(event);
    }
}

void DraggableOverlay::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (isResizing_) {
        QPointF sceneMouse = event->scenePos();
        qreal origW = boundingRect().width();
        qreal origH = boundingRect().height();
        
        QPointF currentPos = pos();
        qreal scaleX = (id_ == "EvalBar") ? transform().m11() : currentScale_;
        qreal scaleY = (id_ == "EvalBar") ? transform().m22() : currentScale_;
        if (scaleX == 0.0) scaleX = 1.0;
        if (scaleY == 0.0) scaleY = 1.0;

        qreal rightEdge = currentPos.x() + origW * scaleX;
        qreal bottomEdge = currentPos.y() + origH * scaleY;
        qreal centerX = currentPos.x() + origW * scaleX / 2.0;
        qreal centerY = currentPos.y() + origH * scaleY / 2.0;
        
        qreal newScaleX = scaleX;
        qreal newScaleY = scaleY;
        QPointF newPos = currentPos;
        
        if (activeHandle_ == BottomRight) {
            newScaleX = (sceneMouse.x() - currentPos.x()) / origW;
            newScaleY = (sceneMouse.y() - currentPos.y()) / origH;
        } else if (activeHandle_ == BottomLeft) {
            newScaleX = (rightEdge - sceneMouse.x()) / origW;
            newScaleY = (sceneMouse.y() - currentPos.y()) / origH;
        } else if (activeHandle_ == TopRight) {
            newScaleX = (sceneMouse.x() - currentPos.x()) / origW;
            newScaleY = (bottomEdge - sceneMouse.y()) / origH;
        } else if (activeHandle_ == TopLeft) {
            newScaleX = (rightEdge - sceneMouse.x()) / origW;
            newScaleY = (bottomEdge - sceneMouse.y()) / origH;
        } else if (activeHandle_ == Right) {
            newScaleX = (sceneMouse.x() - currentPos.x()) / origW;
        } else if (activeHandle_ == Left) {
            newScaleX = (rightEdge - sceneMouse.x()) / origW;
        } else if (activeHandle_ == Bottom) {
            newScaleY = (sceneMouse.y() - currentPos.y()) / origH;
        } else if (activeHandle_ == Top) {
            newScaleY = (bottomEdge - sceneMouse.y()) / origH;
        }
        
        newScaleX = std::clamp(newScaleX, 0.01, 10.0);
        newScaleY = std::clamp(newScaleY, 0.01, 10.0);
        
        if (id_ != "EvalBar") {
            if (activeHandle_ == Left || activeHandle_ == Right) {
                newScaleY = newScaleX;
            } else if (activeHandle_ == Top || activeHandle_ == Bottom) {
                newScaleX = newScaleY;
            } else {
                newScaleX = newScaleY = std::max(newScaleX, newScaleY);
            }
        }
        
        // Re-apply pos based on the mathematically clamped scale to lock anchors safely
        if (activeHandle_ == BottomLeft || activeHandle_ == TopLeft || activeHandle_ == Left) {
             newPos.setX(rightEdge - origW * newScaleX);
        }
        if (activeHandle_ == TopRight || activeHandle_ == TopLeft || activeHandle_ == Top) {
             newPos.setY(bottomEdge - origH * newScaleY);
        }
        
        // Enforce stationary axis centers for edge handles to prevent mouse run-away
        if (id_ != "EvalBar") {
            if (activeHandle_ == Right || activeHandle_ == Left) {
                newPos.setY(centerY - origH * newScaleY / 2.0);
            } else if (activeHandle_ == Bottom || activeHandle_ == Top) {
                newPos.setX(centerX - origW * newScaleX / 2.0);
            }
        }

        if (id_ == "EvalBar") {
            setTransform(QTransform::fromScale(newScaleX, newScaleY));
        } else {
            currentScale_ = newScaleX;
            setScale(newScaleX);
        }
        setPos(newPos);
    } else {
        QGraphicsPixmapItem::mouseMoveEvent(event);
    }
}

void DraggableOverlay::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (isResizing_) {
        isResizing_ = false;
        activeHandle_ = None;
        
        // Snap cursor back to generic hand if released out of bounds
        setCursor(Qt::OpenHandCursor);
        event->accept();
    } else {
        QGraphicsPixmapItem::mouseReleaseEvent(event);
    }
}

void DraggableOverlay::mouseDoubleClickEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        currentScale_ = 1.0;
        if (id_ == "EvalBar") {
            qreal visualSy = videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0;
            setTransform(QTransform::fromScale(1.0, visualSy));
        } else {
            setScale(currentScale_);
        }
        
        // Snap back into bounds if resetting the scale pushed it outside the video area
        if (videoBounds_.isValid()) {
            qreal sx = (id_ == "EvalBar") ? transform().m11() : currentScale_;
            qreal sy = (id_ == "EvalBar") ? transform().m22() : currentScale_;
            qreal maxX = std::max(0.0, videoBounds_.width() - boundingRect().width() * sx);
            qreal maxY = std::max(0.0, videoBounds_.height() - boundingRect().height() * sy);
            setPos(std::clamp(pos().x(), 0.0, maxX), std::clamp(pos().y(), 0.0, maxY));
        }
        event->accept();
    } else {
        QGraphicsPixmapItem::mouseDoubleClickEvent(event);
    }
}

void DraggableOverlay::setVideoBounds(const QSizeF& bounds) {
    videoBounds_ = bounds;
}

void DraggableOverlay::updateFromConfig(const OverlayElement& elem) {
    setVisible(elem.enabled);
    
    qreal scaleY = 1.0;
    if (id_ == "EvalBar") {
        double encoded = elem.scale;
        double sx = std::round(encoded * 100.0) / 100.0;
        double sy = std::round((encoded - sx) * 10000.0 * 100.0) / 100.0;
        if (sy <= 0.0) sy = 1.0;
        
        currentScale_ = sx;
        qreal visualSy = sy * (videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0);
        scaleY = visualSy;
        setTransform(QTransform::fromScale(sx, visualSy));
    } else {
        currentScale_ = elem.scale;
        setScale(currentScale_);
        scaleY = currentScale_;
    }
    
    if (videoBounds_.isValid()) {
        qreal availW = videoBounds_.width() - boundingRect().width() * currentScale_;
        qreal availH = videoBounds_.height() - boundingRect().height() * scaleY;
        setPos(elem.x_percent * std::max(0.0, availW), elem.y_percent * std::max(0.0, availH));
    }
}

void DraggableOverlay::populateConfig(OverlayElement& elem) const {
    elem.enabled = isVisible();
    
    qreal sx = (id_ == "EvalBar") ? transform().m11() : currentScale_;
    qreal sy = (id_ == "EvalBar") ? transform().m22() : currentScale_;
    
    if (id_ == "EvalBar") {
        double logicalSy = sy / (videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0);
        double dsx = std::round(sx * 100.0) / 100.0;
        double dsy = std::round(logicalSy * 100.0) / 100.0;
        // Encode both X and Y into the single scale double (X.XXYYYY)
        elem.scale = dsx + (dsy / 10000.0);
    } else {
        elem.scale = currentScale_;
    }
    
    if (videoBounds_.isValid()) {
        qreal availW = videoBounds_.width() - boundingRect().width() * sx;
        qreal availH = videoBounds_.height() - boundingRect().height() * sy;
        elem.x_percent = (availW > 0) ? std::clamp(x() / availW, 0.0, 1.0) : 0.0;
        elem.y_percent = (availH > 0) ? std::clamp(y() / availH, 0.0, 1.0) : 0.0;
    }
}

QVariant DraggableOverlay::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionChange && scene()) {
        QPointF newPos = value.toPointF();
        if (videoBounds_.isValid()) {
            qreal sx = (id_ == "EvalBar") ? transform().m11() : scale();
            qreal sy = (id_ == "EvalBar") ? transform().m22() : scale();
            if (sx == 0.0) sx = 1.0;
            if (sy == 0.0) sy = 1.0;
            qreal maxX = std::max(0.0, videoBounds_.width() - boundingRect().width() * sx);
            qreal maxY = std::max(0.0, videoBounds_.height() - boundingRect().height() * sy);
            newPos.setX(std::clamp(newPos.x(), 0.0, maxX));
            newPos.setY(std::clamp(newPos.y(), 0.0, maxY));
            return newPos;
        }
    }
    return QGraphicsPixmapItem::itemChange(change, value);
}

void DraggableOverlay::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {
    QGraphicsPixmapItem::paint(painter, option, widget);
    
    if (isSelected()) {
        qreal sx = (id_ == "EvalBar") ? transform().m11() : currentScale_;
        qreal sy = (id_ == "EvalBar") ? transform().m22() : currentScale_;
        if (sx == 0.0) sx = 1.0;
        if (sy == 0.0) sy = 1.0;

        qreal hsX = 20.0 / sx;
        qreal hsY = 20.0 / sy;
        hsX = std::min({hsX, boundingRect().width() / 3.0});
        hsY = std::min({hsY, boundingRect().height() / 3.0});
        QRectF r = boundingRect();
        
        qreal midX = r.center().x() - hsX / 2.0;
        qreal midY = r.center().y() - hsY / 2.0;
        
        painter->setBrush(Qt::white);
        painter->setPen(QPen(Qt::black, 1.0 / std::max(sx, sy)));
        
        painter->drawRect(QRectF(r.left(), r.top(), hsX, hsY));
        painter->drawRect(QRectF(r.right() - hsX, r.top(), hsX, hsY));
        painter->drawRect(QRectF(r.left(), r.bottom() - hsY, hsX, hsY));
        painter->drawRect(QRectF(r.right() - hsX, r.bottom() - hsY, hsX, hsY));
        
        painter->drawRect(QRectF(midX, r.top(), hsX, hsY));
        painter->drawRect(QRectF(midX, r.bottom() - hsY, hsX, hsY));
        painter->drawRect(QRectF(r.left(), midY, hsX, hsY));
        painter->drawRect(QRectF(r.right() - hsX, midY, hsX, hsY));
        
        QPen borderPen(Qt::white, 2.0 / std::max(sx, sy), Qt::DashLine);
        painter->setPen(borderPen);
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(r);
    }
}

// ─── OverlayEditorDialog Implementation ──────────────────────────────────────

OverlayEditorDialog::OverlayEditorDialog(QWidget* parent)
    : QDialog(parent) {
    
    setWindowTitle("Manage Overlay Templates");
    resize(1280, 720);
    setWindowState(windowState() | Qt::WindowMaximized);

    templates_ = cta::TemplateManager::instance().getAllTemplates();
    setupUi();
    setupOverlays();
    
    refreshTemplateCombo();
    if (!templates_.empty()) {
        templateCombo_->setCurrentIndex(0);
    }
}

OverlayEditorDialog::~OverlayEditorDialog() {
}

void OverlayEditorDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (scene_ && !scene_->sceneRect().isEmpty()) {
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
    }
}

void OverlayEditorDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        bool handled = false;
        for (QGraphicsItem* item : scene_->selectedItems()) {
            if (item == boardItem_ && boardCheck_) {
                boardCheck_->setChecked(false);
                handled = true;
            } else if (item == evalBarItem_ && evalCheck_) {
                evalCheck_->setChecked(false);
                handled = true;
            } else if (item == pvTextItem_ && pvCheck_) {
                pvCheck_->setChecked(false);
                handled = true;
            }
        }
        if (handled) return;
    }
    QDialog::keyPressEvent(event);
}

void OverlayEditorDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Toolbar
    auto* topLayout = new QVBoxLayout();
    auto* row1 = new QHBoxLayout();
    row1->addWidget(new QLabel("Template:"));
    templateCombo_ = new QComboBox();
    templateCombo_->setMinimumWidth(200);
    connect(templateCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &OverlayEditorDialog::onTemplateChanged);
    row1->addWidget(templateCombo_);
    
    newTemplateBtn_ = new QPushButton("New Template");
    connect(newTemplateBtn_, &QPushButton::clicked, this, &OverlayEditorDialog::onNewTemplate);
    row1->addWidget(newTemplateBtn_);
    
    deleteTemplateBtn_ = new QPushButton("Delete");
    connect(deleteTemplateBtn_, &QPushButton::clicked, this, &OverlayEditorDialog::onDeleteTemplate);
    row1->addWidget(deleteTemplateBtn_);
    
    auto* reloadBtn = new QPushButton("Reload");
    reloadBtn->setToolTip("Discard unsaved changes and reload templates from disk.");
    connect(reloadBtn, &QPushButton::clicked, this, [this]() {
        cta::TemplateManager::instance().reloadTemplates();
        templates_ = cta::TemplateManager::instance().getAllTemplates();
        refreshTemplateCombo();
        if (!templates_.empty()) templateCombo_->setCurrentIndex(0);
    });
    row1->addWidget(reloadBtn);
    row1->addStretch();
    
    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    btnBox->setToolTip("Save or discard the new overlay configuration.");
    connect(btnBox, &QDialogButtonBox::accepted, this, &OverlayEditorDialog::onAccept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    row1->addWidget(btnBox);
    
    auto* row2 = new QHBoxLayout();
    row2->addWidget(new QLabel("Name:"));
    templateNameEdit_ = new QLineEdit();
    row2->addWidget(templateNameEdit_);
    
    row2->addWidget(new QLabel("Keywords:"));
    templateKeywordsEdit_ = new QLineEdit();
    templateKeywordsEdit_->setToolTip("Optional. The app automatically checks if the video filename contains the template's Name.\nUse this field to add alternative abbreviations or comma-separated keywords if the filename doesn't exactly match the Name.");
    row2->addWidget(templateKeywordsEdit_);
    
    changeScreenshotBtn_ = new QPushButton("Load Reference Screenshot...");
    changeScreenshotBtn_->setToolTip("Load a PNG/JPG from a video to serve as the background canvas for positioning.");
    connect(changeScreenshotBtn_, &QPushButton::clicked, this, &OverlayEditorDialog::onChangeScreenshot);
    row2->addWidget(changeScreenshotBtn_);
    
    topLayout->addLayout(row1);
    topLayout->addLayout(row2);
    mainLayout->addLayout(topLayout);

    // Toggles
    auto* togglesLayout = new QHBoxLayout();
    boardCheck_ = new QCheckBox("Analysis Board");
    boardCheck_->setToolTip("Toggle the visibility of the generated analysis board.");
    evalCheck_ = new QCheckBox("Eval Bar Overlay");
    evalCheck_->setToolTip("Toggle the visibility of the evaluation bar.");
    pvCheck_ = new QCheckBox("PV Text Overlay");
    pvCheck_->setToolTip("Toggle the visibility of the principal variation engine text.");

    togglesLayout->addWidget(boardCheck_);
    togglesLayout->addWidget(evalCheck_);
    togglesLayout->addWidget(pvCheck_);

    auto* arrowsLabel = new QLabel("Engine Arrows:");
    auto* arrowsCombo = new QComboBox();
    arrowsCombo->setObjectName("arrowsCombo");
    arrowsCombo->addItems({"Analysis Board", "Main Board", "Both", "None"});
    arrowsCombo->setToolTip("Select where the engine evaluation arrows should be drawn.");
    togglesLayout->addWidget(arrowsLabel);
    togglesLayout->addWidget(arrowsCombo);

    togglesLayout->addStretch();
    mainLayout->addLayout(togglesLayout);

    connect(boardCheck_, &QCheckBox::toggled, [this](bool checked){ boardItem_->setVisible(checked); if(!checked) boardItem_->setSelected(false); onTogglesChanged(); });
    connect(evalCheck_, &QCheckBox::toggled, [this](bool checked){ evalBarItem_->setVisible(checked); if(!checked) evalBarItem_->setSelected(false); onTogglesChanged(); });
    connect(pvCheck_, &QCheckBox::toggled, [this](bool checked){ pvTextItem_->setVisible(checked); if(!checked) pvTextItem_->setSelected(false); onTogglesChanged(); });
    connect(arrowsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &OverlayEditorDialog::onTogglesChanged);
    
    connect(templateNameEdit_, &QLineEdit::textChanged, this, [this](const QString& text){
        if (currentIndex_ >= 0 && currentIndex_ < templates_.size()) {
            templates_[currentIndex_].name = text;
            templateCombo_->setItemText(currentIndex_, text);
        }
    });

    // Graphics Canvas
    scene_ = new QGraphicsScene(this);
    view_ = new QGraphicsView(scene_);
    view_->setToolTip("Drag and drop the overlays directly on this canvas to set their positions.");
    view_->setStyleSheet("background-color: #2b2b2b;"); // Dark background for contrast
    view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mainLayout->addWidget(view_, 1); // stretch factor 1

    backgroundItem_ = new QGraphicsPixmapItem();
    scene_->addItem(backgroundItem_);
}

void OverlayEditorDialog::setupOverlays() {
    // Generate mock visual representations for the overlays
    QPixmap boardMock(600, 600);
    boardMock.fill(Qt::white);
    QPainter bp(&boardMock);
    bp.setBrush(QColor("#779556"));
    bp.setPen(Qt::NoPen);
    for(int r=0; r<8; ++r) { for(int c=0; c<8; ++c) { if((r+c)%2) bp.drawRect(c*75, r*75, 75, 75); } }
    bp.end();

    QPixmap evalMock(40, 600);
    evalMock.fill(Qt::black);
    QPainter ep(&evalMock);
    ep.setBrush(Qt::white);
    ep.setPen(Qt::NoPen);
    ep.drawRect(0, 300, 40, 300); // 50% advantage
    ep.end();

    QSettings settings;
    int linesPerPosition = settings.value("multiPv", 3).toInt();
    
    QStringList previewLines;
    previewLines << "1. e4 e5 2. Nf3 Nc6 (+0.45)";
    if (linesPerPosition >= 2) previewLines << "1. d4 d5 2. c4 e6 (+0.30)";
    if (linesPerPosition >= 3) previewLines << "1. Nf3 Nf6 2. g3 g6 (+0.15)";
    if (linesPerPosition >= 4) previewLines << "1. c4 c5 2. Nc3 Nc6 (0.00)";
    QString previewString = previewLines.join("\n");

    QPixmap pvMock(800, 40 * std::max(1, linesPerPosition));
    pvMock.fill(QColor(0, 0, 0, 200));
    QPainter pp(&pvMock);
    pp.setPen(Qt::white);
    pp.setFont(QFont("Arial", 16, QFont::Bold));
    pp.drawText(pvMock.rect(), Qt::AlignCenter, previewString);
    pp.end();

    boardItem_ = new DraggableOverlay(boardMock, "Board");
    evalBarItem_ = new DraggableOverlay(evalMock, "EvalBar");
    pvTextItem_ = new DraggableOverlay(pvMock, "PvText");

    scene_->addItem(boardItem_);
    scene_->addItem(evalBarItem_);
    scene_->addItem(pvTextItem_);
    
}

void OverlayEditorDialog::refreshTemplateCombo() {
    templateCombo_->blockSignals(true);
    templateCombo_->clear();
    for (const auto& tpl : templates_) {
        templateCombo_->addItem(tpl.name, tpl.id);
    }
    templateCombo_->blockSignals(false);
}

void OverlayEditorDialog::loadTemplateToUi(int index) {
    if (index < 0 || index >= templates_.size()) return;
    
    // Block signals to prevent onTogglesChanged from corrupting the new config
    templateNameEdit_->blockSignals(true);
    boardCheck_->blockSignals(true);
    evalCheck_->blockSignals(true);
    pvCheck_->blockSignals(true);
    if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
        arrowsCombo->blockSignals(true);
    }

    const auto& tpl = templates_[index];
    templateNameEdit_->setText(tpl.name);
    templateKeywordsEdit_->setText(tpl.keywords.join(", "));
    templateNameEdit_->setEnabled(!tpl.isBuiltIn);
    deleteTemplateBtn_->setEnabled(!tpl.isBuiltIn);
    
    boardCheck_->setChecked(tpl.config.board.enabled);
    evalCheck_->setChecked(tpl.config.evalBar.enabled);
    pvCheck_->setChecked(tpl.config.pvText.enabled);
    
    QString screenshotPath = cta::TemplateManager::instance().getScreenshotPath(tpl.screenshotFilename);
    QPixmap bg(screenshotPath);
    if (bg.isNull()) {
        bg = QPixmap(1920, 1080);
        bg.fill(QColor("#2b2b2b"));
    }
    backgroundItem_->setPixmap(bg);
    scene_->setSceneRect(0, 0, bg.width(), bg.height());
    view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
    
    QSizeF bounds = bg.size();
    boardItem_->setVideoBounds(bounds);
    evalBarItem_->setVideoBounds(bounds);
    pvTextItem_->setVideoBounds(bounds);

    boardItem_->updateFromConfig(tpl.config.board);
    if (!tpl.config.board.enabled) boardItem_->setSelected(false);

    evalBarItem_->updateFromConfig(tpl.config.evalBar);
    if (!tpl.config.evalBar.enabled) evalBarItem_->setSelected(false);

    pvTextItem_->updateFromConfig(tpl.config.pvText);
    if (!tpl.config.pvText.enabled) pvTextItem_->setSelected(false);

    if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
        int aIdx = arrowsCombo->findText(QString::fromStdString(tpl.config.arrowsTarget));
        arrowsCombo->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
        arrowsCombo->blockSignals(false);
    }
    
    templateNameEdit_->blockSignals(false);
    boardCheck_->blockSignals(false);
    evalCheck_->blockSignals(false);
    pvCheck_->blockSignals(false);
}

void OverlayEditorDialog::saveUiToTemplate(int index) {
    if (index < 0 || index >= templates_.size()) return;
    auto& tpl = templates_[index];
    
    QString kws = templateKeywordsEdit_->text();
    QStringList kwList = kws.split(",", Qt::SkipEmptyParts);
    for (auto& kw : kwList) kw = kw.trimmed();
    tpl.keywords = kwList;
    
    boardItem_->populateConfig(tpl.config.board);
    evalBarItem_->populateConfig(tpl.config.evalBar);
    pvTextItem_->populateConfig(tpl.config.pvText);

    if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
        tpl.config.arrowsTarget = arrowsCombo->currentText().toStdString();
    }
}

void OverlayEditorDialog::updateOverlayBounds() {
    QSizeF bounds = scene_->sceneRect().size();
    if (!bounds.isValid() || bounds.isEmpty()) return;
    
    if (currentIndex_ >= 0) {
        saveUiToTemplate(currentIndex_);
        loadTemplateToUi(currentIndex_);
    }
}

void OverlayEditorDialog::onTemplateChanged(int index) {
    if (currentIndex_ >= 0 && currentIndex_ < templates_.size()) {
        saveUiToTemplate(currentIndex_);
    }
    currentIndex_ = index;
    if (currentIndex_ >= 0) {
        loadTemplateToUi(currentIndex_);
    }
}

void OverlayEditorDialog::onNewTemplate() {
    OverlayTemplate newTpl;
    newTpl.id = "custom_" + QString::number(QDateTime::currentMSecsSinceEpoch());
    newTpl.name = "New Custom Template";
    newTpl.isBuiltIn = false;
    newTpl.config = cta::TemplateManager::instance().getFallbackTemplate().config;
    
    templates_.push_back(newTpl);
    refreshTemplateCombo();
    templateCombo_->setCurrentIndex(templates_.size() - 1);
}

void OverlayEditorDialog::onDeleteTemplate() {
    if (currentIndex_ < 0 || currentIndex_ >= templates_.size()) return;
    if (templates_[currentIndex_].isBuiltIn) return;
    
    auto reply = QMessageBox::question(this, "Confirm Delete", "Are you sure you want to delete this custom template?", QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        deletedTemplateIds_.push_back(templates_[currentIndex_].id);
        templates_.erase(templates_.begin() + currentIndex_);
        currentIndex_ = -1;
        refreshTemplateCombo();
        if (!templates_.empty()) templateCombo_->setCurrentIndex(0);
        else {
            backgroundItem_->setPixmap(QPixmap());
            templateNameEdit_->clear();
            templateKeywordsEdit_->clear();
        }
    }
}

void OverlayEditorDialog::onChangeScreenshot() {
    if (currentIndex_ < 0) return;
    QString path = QFileDialog::getOpenFileName(this, "Select Reference Screenshot", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), "Images (*.png *.jpg *.jpeg)");
    if (!path.isEmpty()) {
        QString newFileName = templates_[currentIndex_].id + "_ref.png";
        QString destPath = cta::TemplateManager::instance().getScreenshotPath(newFileName);
        if (QFile::exists(destPath)) QFile::remove(destPath);
        QFile::copy(path, destPath);
        
        templates_[currentIndex_].screenshotFilename = newFileName;
        loadTemplateToUi(currentIndex_);
    }
}

void OverlayEditorDialog::onTogglesChanged() {
    if (currentIndex_ >= 0 && currentIndex_ < templates_.size()) {
        templates_[currentIndex_].config.board.enabled = boardItem_ && boardItem_->isVisible();
        templates_[currentIndex_].config.evalBar.enabled = evalBarItem_ && evalBarItem_->isVisible();
        templates_[currentIndex_].config.pvText.enabled = pvTextItem_ && pvTextItem_->isVisible();
        if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
            templates_[currentIndex_].config.arrowsTarget = arrowsCombo->currentText().toStdString();
        }
    }
}

void OverlayEditorDialog::onAccept() {
    if (currentIndex_ >= 0) {
        saveUiToTemplate(currentIndex_);
    }
    
    for (const QString& id : deletedTemplateIds_) {
        cta::TemplateManager::instance().deleteTemplate(id);
    }
    for (const auto& tpl : templates_) {
        cta::TemplateManager::instance().saveTemplate(tpl);
    }
    
    accept();
}

} // namespace cta
