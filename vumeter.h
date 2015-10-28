#ifndef VUMETER_H
#define VUMETER_H

#include <QWidget>
#include <QPaintEvent>

#include <mutex>

class VUMeter : public QWidget
{
	Q_OBJECT

public:
	VUMeter(QWidget *parent);

	void set_level(float level) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level = level;
		update();
	}

private:
	void paintEvent(QPaintEvent *event) override;

	std::mutex level_mutex;
	float level = -HUGE_VAL;
};

extern VUMeter *global_vu_meter;

#endif
