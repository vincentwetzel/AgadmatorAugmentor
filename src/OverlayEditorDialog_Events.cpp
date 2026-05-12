#include "OverlayEditorDialog.h"

#include <QKeyEvent>
#include <QResizeEvent>

namespace cta {

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
            } else if (item == openingTextItem_ && openingCheck_) {
                openingCheck_->setChecked(false);
                handled = true;
            }
        }
        if (handled) return;
    }
    QDialog::keyPressEvent(event);
}


} // namespace cta
