#ifndef _VU_COMMON_H
#define _VU_COMMON_H 1

#include <QPainter>

int lufs_to_pos(float level_lu, int height);

// TODO: Now that we precalculate these as pixmaps, perhaps we don't need the
// high/low range anymore, just a yes/no.
void draw_vu_meter(QPainter &painter, float range_low_lu, float range_high_lu, int width, int height, int margin);

#endif // !defined(_VU_COMMON_H)
