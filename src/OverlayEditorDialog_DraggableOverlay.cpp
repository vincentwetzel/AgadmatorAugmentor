#include "OverlayEditorDialog.h"

#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

namespace cta {

// ─── DraggableOverlay Implementation ─────────────────────────────────────────

DraggableOverlay::DraggableOverlay(const QPixmap& pixmap, const QString& id, QGraphicsItem* parent)
    : QGraphicsPixmapItem(pixmap, parent), id_(id) {
    setFlags(QGraphicsItem::ItemIsSelectable | QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
}

DraggableOverlay::ResizeHandle DraggableOverlay::getHandleAt(const QPointF& pos) const {
    qreal sx = (id_ == "EvalBar") ? transform().m11() : scale();
    qreal sy = (id_ == "EvalBar") ? transform().m22() : scale();
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
        qreal scaleX = (id_ == "EvalBar") ? transform().m11() : scale();
        qreal scaleY = (id_ == "EvalBar") ? transform().m22() : scale();
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
        qreal videoScaleRatio = videoBounds_.isValid() ? (videoBounds_.height() / 1080.0) : 1.0;
        if (id_ == "EvalBar") {
            qreal visualSy = videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0;
            setTransform(QTransform::fromScale(videoScaleRatio, visualSy));
        } else {
            setScale(videoScaleRatio);
        }
        
        // Snap back into bounds if resetting the scale pushed it outside the video area
        if (videoBounds_.isValid()) {
            qreal sx = (id_ == "EvalBar") ? transform().m11() : scale();
            qreal sy = (id_ == "EvalBar") ? transform().m22() : scale();
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
    
    qreal videoScaleRatio = videoBounds_.isValid() ? (videoBounds_.height() / 1080.0) : 1.0;
    qreal visualSx = 1.0;
    qreal visualSy = 1.0;
    
    if (id_ == "EvalBar") {
        double encoded = elem.scale;
        double sx = std::round(encoded * 100.0) / 100.0;
        double sy = std::round((encoded - sx) * 10000.0 * 100.0) / 100.0;
        if (sy <= 0.0) sy = 1.0;
        
        visualSx = sx * videoScaleRatio;
        visualSy = sy * (videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0);
        setTransform(QTransform::fromScale(visualSx, visualSy));
    } else {
        visualSx = elem.scale * videoScaleRatio;
        visualSy = visualSx;
        setScale(visualSx);
    }
    
    if (videoBounds_.isValid()) {
        qreal availW = videoBounds_.width() - boundingRect().width() * visualSx;
        qreal availH = videoBounds_.height() - boundingRect().height() * visualSy;
        setPos(elem.x_percent * std::max(0.0, availW), elem.y_percent * std::max(0.0, availH));
    }
}

void DraggableOverlay::populateConfig(OverlayElement& elem) const {
    elem.enabled = isVisible();
    
    qreal videoScaleRatio = videoBounds_.isValid() ? (videoBounds_.height() / 1080.0) : 1.0;
    qreal sx = (id_ == "EvalBar") ? transform().m11() : scale();
    qreal sy = (id_ == "EvalBar") ? transform().m22() : scale();
    
    if (id_ == "EvalBar") {
        double logicalSy = sy / (videoBounds_.isValid() ? (videoBounds_.height() / boundingRect().height()) : 1.0);
        double dsx = std::round((sx / videoScaleRatio) * 100.0) / 100.0;
        double dsy = std::round(logicalSy * 100.0) / 100.0;
        // Encode both X and Y into the single scale double (X.XXYYYY)
        elem.scale = dsx + (dsy / 10000.0);
    } else {
        elem.scale = sx / videoScaleRatio;
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
        qreal sx = (id_ == "EvalBar") ? transform().m11() : scale();
        qreal sy = (id_ == "EvalBar") ? transform().m22() : scale();
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

} // namespace cta
