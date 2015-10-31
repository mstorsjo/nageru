#include <QPainter>

#include "lrameter.h"
#include "vu_common.h"

using namespace std;

LRAMeter::LRAMeter(QWidget *parent)
	: QWidget(parent)
{
}

void LRAMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());

	float level_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	const int margin = 5;
	draw_vu_meter(painter, level_lu, width(), height(), margin);

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
