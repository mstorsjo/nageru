// TODO: This isn't really an LRA meter right now (it ignores the range).

#ifndef LRAMETER_H
#define LRAMETER_H

#include <math.h>
#include <QLabel>
#include <QPaintEvent>
#include <QWidget>
#include <mutex>

class LRAMeter : public QWidget
{
	Q_OBJECT

public:
	LRAMeter(QWidget *parent);

	void set_levels(float level_lufs, float range_low_lufs, float range_high_lufs) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level_lufs = level_lufs;
		this->range_low_lufs = range_low_lufs;
		this->range_high_lufs = range_high_lufs;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;

	std::mutex level_mutex;
	float level_lufs = -HUGE_VAL;
	float range_low_lufs = -HUGE_VAL;
	float range_high_lufs = -HUGE_VAL;

	QPixmap on_pixmap, off_pixmap;
};

#endif
