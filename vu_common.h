#ifndef _VU_COMMON_H
#define _VU_COMMON_H 1

#include <QPainter>

int lufs_to_pos(float level_lu, int height);
void draw_vu_meter(QPainter &painter, float level_lu, int width, int height, int margin);

#endif // !defined(_VU_COMMON_H)
