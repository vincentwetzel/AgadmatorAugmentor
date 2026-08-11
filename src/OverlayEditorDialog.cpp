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
#include <QCoreApplication>
#include <QFileInfo>
#include <QDir>
#include <QSpinBox>


namespace cta {

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

void OverlayEditorDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);

    // Toolbar
    auto* topLayout = new QVBoxLayout();
    auto* row1 = new QHBoxLayout();
    row1->addWidget(new QLabel("Template:"));
    templateCombo_ = new QComboBox();
    templateCombo_->setMinimumWidth(200);
    templateCombo_->setToolTip("Select an overlay template to edit or view.");
    connect(templateCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &OverlayEditorDialog::onTemplateChanged);
    row1->addWidget(templateCombo_);
    
    newTemplateBtn_ = new QPushButton("New Template");
    newTemplateBtn_->setToolTip("Create a new custom overlay template.");
    connect(newTemplateBtn_, &QPushButton::clicked, this, &OverlayEditorDialog::onNewTemplate);
    row1->addWidget(newTemplateBtn_);
    
    deleteTemplateBtn_ = new QPushButton("Delete");
    deleteTemplateBtn_->setToolTip("Delete the currently selected custom template.");
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
    templateNameEdit_->setToolTip("The display name for the template. Built-in templates cannot be renamed.");
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
    openingCheck_ = new QCheckBox("Opening Text Overlay");
    openingCheck_->setToolTip("Toggle the visibility of the opening name text.");

    togglesLayout->addWidget(boardCheck_);
    togglesLayout->addWidget(evalCheck_);
    togglesLayout->addWidget(pvCheck_);
    togglesLayout->addWidget(openingCheck_);

    auto* arrowsLabel = new QLabel("Engine Arrows:");
    auto* arrowsCombo = new QComboBox();
    arrowsCombo->setObjectName("arrowsCombo");
    arrowsCombo->addItems({"Analysis Board", "Main Board", "Both", "None"});
    arrowsCombo->setToolTip("Select where the engine evaluation arrows should be drawn.");
    togglesLayout->addWidget(arrowsLabel);
    togglesLayout->addWidget(arrowsCombo);

    auto* arrowsThicknessLabel = new QLabel("Arrow Thickness (%):");
    arrowsThicknessLabel->setToolTip("Adjust the base thickness percentage of engine arrows.");
    auto* arrowsThicknessSpin = new QSpinBox();
    arrowsThicknessSpin->setObjectName("arrowsThicknessSpin");
    arrowsThicknessSpin->setRange(5, 40);
    arrowsThicknessSpin->setToolTip("Adjust the base thickness percentage of engine arrows.");
    togglesLayout->addWidget(arrowsThicknessLabel);
    togglesLayout->addWidget(arrowsThicknessSpin);

    togglesLayout->addStretch();
    mainLayout->addLayout(togglesLayout);

    connect(boardCheck_, &QCheckBox::toggled, [this](bool checked){ boardItem_->setVisible(checked); if(!checked) boardItem_->setSelected(false); onTogglesChanged(); });
    connect(evalCheck_, &QCheckBox::toggled, [this](bool checked){ evalBarItem_->setVisible(checked); if(!checked) evalBarItem_->setSelected(false); onTogglesChanged(); });
    connect(pvCheck_, &QCheckBox::toggled, [this](bool checked){ pvTextItem_->setVisible(checked); if(!checked) pvTextItem_->setSelected(false); onTogglesChanged(); });
    connect(openingCheck_, &QCheckBox::toggled, [this](bool checked){ openingTextItem_->setVisible(checked); if(!checked) openingTextItem_->setSelected(false); onTogglesChanged(); });
    connect(arrowsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &OverlayEditorDialog::onTogglesChanged);
    connect(arrowsThicknessSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &OverlayEditorDialog::onTogglesChanged);
    
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
    QString assetPath = "assets/reference/board/board.png";
    if (!QFileInfo::exists(assetPath)) {
        assetPath = QDir(QCoreApplication::applicationDirPath()).filePath("../../assets/reference/board/board.png");
    }
    QPixmap boardMock(assetPath);
    if (boardMock.isNull()) {
        boardMock = QPixmap(1080, 1080);
        boardMock.fill(Qt::white);
        QPainter bp(&boardMock);
        bp.setBrush(QColor("#779556"));
        bp.setPen(Qt::NoPen);
        for(int r=0; r<8; ++r) { for(int c=0; c<8; ++c) { if((r+c)%2) bp.drawRect(c*(1080/8), r*(1080/8), 1080/8, 1080/8); } }
        bp.end();
    }

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

    QPixmap openingMock(800, 40);
    openingMock.fill(QColor(0, 0, 0, 200));
    QPainter op(&openingMock);
    op.setPen(Qt::white);
    op.setFont(QFont("Arial", 16, QFont::Bold));
    op.drawText(openingMock.rect(), Qt::AlignCenter, "C42 Petrov's Defense");
    op.end();

    boardItem_ = new DraggableOverlay(boardMock, "Board");
    evalBarItem_ = new DraggableOverlay(evalMock, "EvalBar");
    pvTextItem_ = new DraggableOverlay(pvMock, "PvText");
    openingTextItem_ = new DraggableOverlay(openingMock, "OpeningText");

    scene_->addItem(boardItem_);
    scene_->addItem(evalBarItem_);
    scene_->addItem(pvTextItem_);
    scene_->addItem(openingTextItem_);
    
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
    openingCheck_->blockSignals(true);
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
    openingCheck_->setChecked(tpl.config.openingText.enabled);
    
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
    openingTextItem_->setVideoBounds(bounds);

    boardItem_->updateFromConfig(tpl.config.board);
    if (!tpl.config.board.enabled) boardItem_->setSelected(false);

    evalBarItem_->updateFromConfig(tpl.config.evalBar);
    if (!tpl.config.evalBar.enabled) evalBarItem_->setSelected(false);

    pvTextItem_->updateFromConfig(tpl.config.pvText);
    if (!tpl.config.pvText.enabled) pvTextItem_->setSelected(false);

    openingTextItem_->updateFromConfig(tpl.config.openingText);
    if (!tpl.config.openingText.enabled) openingTextItem_->setSelected(false);

    if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
        int aIdx = arrowsCombo->findText(QString::fromStdString(tpl.config.arrowsTarget));
        arrowsCombo->setCurrentIndex(aIdx >= 0 ? aIdx : 0);
        arrowsCombo->blockSignals(false);
    }
    if (auto* arrowsThicknessSpin = findChild<QSpinBox*>("arrowsThicknessSpin")) {
        arrowsThicknessSpin->setValue(tpl.config.arrowThicknessPct);
        arrowsThicknessSpin->blockSignals(false);
    }
    
    templateNameEdit_->blockSignals(false);
    boardCheck_->blockSignals(false);
    evalCheck_->blockSignals(false);
    pvCheck_->blockSignals(false);
    openingCheck_->blockSignals(false);
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
    openingTextItem_->populateConfig(tpl.config.openingText);

    if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
        tpl.config.arrowsTarget = arrowsCombo->currentText().toStdString();
    }
    if (auto* arrowsThicknessSpin = findChild<QSpinBox*>("arrowsThicknessSpin")) {
        tpl.config.arrowThicknessPct = arrowsThicknessSpin->value();
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
        templates_[currentIndex_].config.openingText.enabled = openingTextItem_ && openingTextItem_->isVisible();
        if (auto* arrowsCombo = findChild<QComboBox*>("arrowsCombo")) {
            templates_[currentIndex_].config.arrowsTarget = arrowsCombo->currentText().toStdString();
        }
        if (auto* arrowsThicknessSpin = findChild<QSpinBox*>("arrowsThicknessSpin")) {
            templates_[currentIndex_].config.arrowThicknessPct = arrowsThicknessSpin->value();
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
