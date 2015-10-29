#include <QPainter>

#include "vumeter.h"

using namespace std;

namespace {

int lufs_to_pos(float level_lu, int height)
{
	const float min_level = 18.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -36.0f;
	int y = lrintf(height * (level_lu - min_level) / (max_level - min_level));
	y = std::max(y, 0);
	y = std::min(y, height - 1);
	return y;
}

}  // namespace

VUMeter *global_vu_meter = nullptr;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	painter.fillRect(0, 0, width(), height(), Qt::black);

	int min_y = lufs_to_pos(1.0f, height());
	int max_y = lufs_to_pos(-1.0f, height());
	painter.fillRect(0, min_y, width(), max_y - min_y, Qt::green);

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	int y = lufs_to_pos(level_lu, height());
	painter.setPen(Qt::white);
	painter.drawLine(0, y, width(), y);
}
