#ifndef _TIMEBASE_H
#define _TIMEBASE_H 1

// Common timebase that allows us to represent one frame exactly in all the
// relevant frame rates:
//
//   Timebase:              1/60000
//   Frame at 50fps:     1200/60000
//   Frame at 60fps:     1000/60000
//   Frame at 59.94fps:  1001/60000
//
// If we also wanted to represent one sample at 48000 Hz, we'd need
// to go to 300000. Also supporting one sample at 44100 Hz would mean
// going to 44100000; probably a bit excessive.
#define TIMEBASE 60000

#endif  // !defined(_TIMEBASE_H)
