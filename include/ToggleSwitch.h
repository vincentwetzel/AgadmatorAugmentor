#pragma once

#include <QWidget>
#include <QString>

class ToggleSwitch : public QWidget {
    Q_OBJECT

public:
    ToggleSwitch(const QString& label = "", bool checked = false, QWidget* parent = nullptr);

    bool isChecked() const;
    void setChecked(bool checked);

    QString label() const;
    void setLabel(const QString& label);

signals:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool checked_;
    QString label_;
};
