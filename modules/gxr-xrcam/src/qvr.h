// Minimal reverse-engineered interface to Qualcomm QVR camera client
// (/vendor/lib64/libqvrcamera_client.qti.so) as found on Samsung Galaxy XR.
#pragma once
#include <stdint.h>

typedef struct {
    int32_t api_version;   // client iface = 9, device iface = 10 on I610UEU2AZD8
    int32_t pad;
    void  **ops;
} qvr_iface_t;

// client ops (camera_client_ops table)
enum {
    QC_CREATE = 0,               // void* (*)(void)
    QC_DESTROY = 1,              // void  (*)(void* client)
    QC_SET_STATUS_CB = 2,
    QC_ATTACH = 3,               // void* (*)(void* client, const char* name)
    QC_CREATE_WITH_KEY = 4,
    QC_INIT_RING = 5,
    QC_WRITE_RING = 6,
    QC_GET_PARAM = 7,
    QC_SET_PARAM = 8,
    QC_REG_NOTIFY = 9,
    QC_ATTACH_WITH_PARAMS = 10,
    QC_ENUMERATE = 11,
    QC_SYSINFO = 12,
};

// device ops (camera_device_ops table)
enum {
    QD_DETACH = 0,               // void (*)(void* dev)
    QD_GET_STATE = 1,            // int (*)(void* dev, int* state)  3=STARTING 4=STARTED
    QD_SET_STATE_CB = 2,
    QD_GET_PARAM = 3,            // int (*)(void* dev, const char* name, uint32_t* len, char* out)
    QD_SET_PARAM = 4,            // int (*)(void* dev, const char* name, const char* val)
    QD_GET_PARAM_NUM = 5,        // int (*)(void* dev, const char* name, int type, uint8_t size, void* out)
    QD_SET_PARAM_NUM = 6,
    QD_START = 7,                // int (*)(void* dev)
    QD_STOP = 8,                 // int (*)(void* dev)
    QD_CUR_FN = 9,               // int (*)(void* dev, int* fn)
    QD_SET_EXP_GAIN = 10,        // int (*)(void* dev, uint64_t exposure_ns, int gain)
    QD_GET_FRAME = 11,           // int (*)(void* dev, int* fn, int block, int drop, qvr_frame_t* f)
    QD_RELEASE_FRAME = 12,       // int (*)(void* dev, int fn)
    QD_SET_CROP = 13,
    QD_REG_NOTIFY = 14,
    QD_SET_GAMMA = 15,
    QD_GET_PROPS = 18,
    QD_GET_SYNC = 19,
    QD_REL_SYNC = 20,
    QD_FRAME_TO_HWB = 21,        // int (*)(void* dev, qvr_frame_t* f, uint32_t* n, qvr_hwbuf_t** out)
    QD_GET_FRAME_PARAM_NUM = 22,
    QD_GET_CLASS = 23,
    QD_REL_CLASS = 24,
    QD_GET_FRAME_EX = 25,
    QD_GET_PROPS_EX = 26,
    QD_GET_PRIV = 27,
    QD_REL_PRIV = 28,
};

// qvrcamera_frame_t (partial, observed layout)
typedef struct {
    uint32_t fn;          // 0x00 frame number
    uint32_t pad0;
    uint64_t ts_ns;       // 0x08 start of exposure (boottime ns)
    uint32_t exposure_ns; // 0x10
    uint32_t pad1;
    uint8_t *buffer;      // 0x18 (NULL on this platform; use FrameToHardwareBuffer)
    uint32_t len;         // 0x20
    uint32_t r24[4];
    uint32_t gain;        // 0x34
    uint8_t  rest[456];   // generous padding
} qvr_frame_t;

typedef struct {
    void    *ahb;         // AHardwareBuffer*
    uint32_t off_x;       // x offset of this sub-camera inside the logical frame
    uint32_t off_y;
} qvr_hwbuf_t;

// QVR error codes seen
#define QVR_CAM_DROPPED_FRAMENUMBER (-5)
#define QVR_CAM_EXPIRED_FRAMENUMBER (-7)
