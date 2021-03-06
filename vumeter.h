#ifndef VUMETER_H
#define VUMETER_H

#include <math.h>
#include <QLabel>
#include <QPaintEvent>
#include <QWidget>
#include <mutex>

class VUMeter : public QWidget
{
	Q_OBJECT

public:
	VUMeter(QWidget *parent);

	void set_level(float level_lufs) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level_lufs = level_lufs;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

	std::mutex level_mutex;
	float level_lufs = -HUGE_VAL;

	QPixmap on_pixmap, off_pixmap;
};

#endif
