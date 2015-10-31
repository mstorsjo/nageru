#include <QPainter>

#include "lrameter.h"

using namespace std;

namespace {

int lufs_to_pos(float level_lu, int height)
{
	const float min_level = 9.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -18.0f;

	// Handle -inf.
	if (level_lu < max_level) {
		return height - 1;
	}

	int y = lrintf(height * (level_lu - min_level) / (max_level - min_level));
	y = std::max(y, 0);
	y = std::min(y, height - 1);
	return y;
}

}  // namespace

LRAMeter::LRAMeter(QWidget *parent)
	: QWidget(parent)
{
}

void LRAMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);
	const int margin = 5;

	painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	painter.fillRect(margin, 0, width() - 2 * margin, height(), Qt::black);

	// TODO: QLinearGradient is not gamma-correct; we might want to correct for that.
	QLinearGradient on(0, 0, 0, height());
        on.setColorAt(0.0f, QColor(255, 0, 0));
        on.setColorAt(0.5f, QColor(255, 255, 0));
        on.setColorAt(1.0f, QColor(0, 255, 0));
	QColor off(80, 80, 80);

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	int y = lufs_to_pos(level_lu, height());

	// Draw bars colored up until the level, then gray from there.
	for (int level = -18; level < 9; ++level) {
		int min_y = lufs_to_pos(level + 1.0f, height()) + 1;
		int max_y = lufs_to_pos(level, height()) - 1;

		if (y > max_y) {
			painter.fillRect(margin, min_y, width() - 2 * margin, max_y - min_y, off);
		} else if (y < min_y) {
			painter.fillRect(margin, min_y, width() - 2 * margin, max_y - min_y, on);
		} else {
			painter.fillRect(margin, min_y, width() - 2 * margin, y - min_y, off);
			painter.fillRect(margin, y, width() - 2 * margin, max_y - y, on);
		}
	}

	// Draw the target area (+/-1 LU is allowed EBU range).
	int min_y = lufs_to_pos(1.0f, height());
	int max_y = lufs_to_pos(-1.0f, height());

	// FIXME: This outlining isn't so pretty.
	{
		QPen pen(Qt::black);
		pen.setWidth(5);
		painter.setPen(pen);
		painter.drawRect(2, min_y, width() - 5, max_y - min_y);
	}
	{
		QPen pen;
		if (level_lu >= -1.0f && level_lu <= 1.0f) {
			pen.setColor(Qt::green);
		} else {
			pen.setColor(Qt::red);
		}
		pen.setWidth(3);
		painter.setPen(pen);
		painter.drawRect(2, min_y, width() - 5, max_y - min_y);
	}
}
