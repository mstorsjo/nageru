#include <QPainter>

#include "vumeter.h"
#include "vu_common.h"

using namespace std;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	draw_vu_meter(painter, -HUGE_VAL, level_lu, width(), height(), 0);
}
