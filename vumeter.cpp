#include <QPainter>

#include "vumeter.h"

using namespace std;

namespace {

int lufs_to_pos(float level_lu, int height)
{
	const float min_level = 9.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -18.0f;
	int y = lrintf(height * (level_lu - min_level) / (max_level - min_level));
	y = std::max(y, 0);
	y = std::min(y, height - 1);
	return y;
}

}  // namespace

VUMeter *global_vu_meter = nullptr;
QLabel *global_peak_display = nullptr;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	painter.fillRect(0, 0, width(), height(), Qt::black);

	// Draw some reference bars.
	for (int level = -18; level < 9; ++level) {
		int min_y = lufs_to_pos(level, height()) - 1;
		int max_y = lufs_to_pos(level + 1.0f, height()) + 1;

		// Recommended range is 0 LU +/- 1 LU.
		if (level == -1 || level == 0) {
			painter.fillRect(1, min_y, width() - 2, max_y - min_y, Qt::green);
		} else {
			painter.fillRect(1, min_y, width() - 2, max_y - min_y, Qt::darkGray);
		}
	}

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	int y = lufs_to_pos(level_lu, height());
	painter.fillRect(0, y, width(), 2, Qt::white);
}
