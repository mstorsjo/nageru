#include <QPainter>

#include "vumeter.h"

using namespace std;

VUMeter *global_vu_meter = nullptr;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	painter.fillRect(0, 0, width(), height(), Qt::black);

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	const float min_level = 18.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -36.0f;
	int y = lrintf(height() * (level_lu - min_level) / (max_level - min_level));
	if (y >= 0 && y < height()) {
		painter.setPen(Qt::white);
		painter.drawLine(0, y, width(), y);
	}
}
