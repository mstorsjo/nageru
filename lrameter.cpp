#include "lrameter.h"

#include <QPainter>
#include <QPalette>
#include <QPen>
#include "vu_common.h"

class QPaintEvent;

using namespace std;

LRAMeter::LRAMeter(QWidget *parent)
	: QWidget(parent)
{
}

void LRAMeter::resizeEvent(QResizeEvent *event)
{
	const int margin = 5;

	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	on_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(on_painter, -HUGE_VAL, HUGE_VAL, width(), height(), margin);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	off_painter.fillRect(0, 0, width(), height(), parentWidget()->palette().window());
	draw_vu_meter(off_painter, -HUGE_VAL, -HUGE_VAL, width(), height(), margin);
}

void LRAMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_lufs;
	float range_low_lufs;
	float range_high_lufs;
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs = this->level_lufs;
		range_low_lufs = this->range_low_lufs;
		range_high_lufs = this->range_high_lufs;
	}

	float level_lu = level_lufs + 23.0f;
	float range_low_lu = range_low_lufs + 23.0f;
	float range_high_lu = range_high_lufs + 23.0f;
	int range_low_pos = lufs_to_pos(range_low_lu, height());
	int range_high_pos = lufs_to_pos(range_high_lu, height());

	QRect top_off_rect(0, 0, width(), range_high_pos);
	QRect on_rect(0, range_high_pos, width(), range_low_pos - range_high_pos);
	QRect bottom_off_rect(0, range_low_pos, width(), height() - range_low_pos);

	painter.drawPixmap(top_off_rect, off_pixmap, top_off_rect);
	painter.drawPixmap(on_rect, on_pixmap, on_rect);
	painter.drawPixmap(bottom_off_rect, off_pixmap, bottom_off_rect);

	// Draw the target area (+/-1 LU is allowed EBU range).
	// It turns green when we're within.
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

	// Draw the integrated loudness meter, in the same color as the target area.
	int y = lufs_to_pos(level_lu, height());
	{
		QPen pen(Qt::black);
		pen.setWidth(5);
		painter.setPen(pen);
		painter.drawRect(2, y, width() - 5, 1);
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
		painter.drawRect(2, y, width() - 5, 1);
	}
}
