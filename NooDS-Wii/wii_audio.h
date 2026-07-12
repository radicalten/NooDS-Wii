#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIIAUD_OUT_RATE         48000
#define WIIAUD_FRAMES_PER_BUF   1024
#define WIIAUD_BUF_BYTES_STEREO (WIIAUD_FRAMES_PER_BUF * 4)
#define WIIAUD_BUF_BYTES_MONO   (WIIAUD_FRAMES_PER_BUF * 2)

#define WIIAUD_NDS_RATE              32768
#define WIIAUD_NDS_FRAMES_PER_VFRAME 548
#define WIIAUD_NDS_FRAMES            WIIAUD_NDS_FRAMES_PER_VFRAME

#define WIIAUD_GBA_RATE              32768
#define WIIAUD_GBA_FRAMES_PER_VFRAME 549
#define WIIAUD_GBA_FRAMES            WIIAUD_GBA_FRAMES_PER_VFRAME

#ifdef __cplusplus
}
#endif
