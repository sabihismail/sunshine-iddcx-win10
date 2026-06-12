/*
 * libvoidrv - user-mode SDK for the Void drivers.
 *
 * Public, self-contained C ABI. A host app links libvoidrv and talks to the
 * drivers through these functions; it never builds raw IOCTL buffers. C++ is
 * fine too - everything is declared extern "C".
 *
 * This header intentionally does not expose the driver wire format (that lives
 * in the driver's Public.h); the SDK ABI is decoupled from it.
 */

#ifndef VOIDRV_H
#define VOIDRV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOIDRV_MAX_DISPLAYS 8

typedef struct VoidrvDisplayMode {
    uint32_t Width;      /* pixels; 0 in an Add request selects the driver default */
    uint32_t Height;     /* pixels */
    uint32_t RefreshHz;  /* nominal refresh, e.g. 60, 120, 144, 240 */
} VoidrvDisplayMode;

typedef struct VoidrvDisplayEntry {
    uint32_t          InUse;
    VoidrvDisplayMode Mode;
} VoidrvDisplayEntry;

typedef struct VoidrvDisplayState {
    uint32_t           Count;
    VoidrvDisplayEntry Entries[VOIDRV_MAX_DISPLAYS];
} VoidrvDisplayState;

typedef enum VoidrvStatus {
    VOIDRV_STATUS_OK = 0,        /* present and openable */
    VOIDRV_STATUS_NOT_INSTALLED, /* device interface not found */
    VOIDRV_STATUS_INACCESSIBLE   /* found but could not be opened */
} VoidrvStatus;

/* Opaque control-channel handle. */
typedef struct VoidrvDisplay* VoidrvDisplayHandle;

/* Is the VoidDisplay control interface present? */
VoidrvStatus        VoidrvDisplayQueryStatus(void);

/* Open / close the control channel. Open returns NULL on failure (GetLastError). */
VoidrvDisplayHandle VoidrvDisplayOpen(void);
void                VoidrvDisplayClose(VoidrvDisplayHandle handle);

/* Driver interface version, or 0 on failure. */
uint32_t            VoidrvDisplayVersion(VoidrvDisplayHandle handle);

/* Add a display. Pass NULL or a zeroed mode for the driver default. */
/* Returns the new slot index (>= 0), or -1 on failure. */
int                 VoidrvDisplayAdd(VoidrvDisplayHandle handle, const VoidrvDisplayMode* mode);

/* Remove a display by slot index. */
bool                VoidrvDisplayRemove(VoidrvDisplayHandle handle, uint32_t index);

/* Change a display's mode. */
bool                VoidrvDisplaySetMode(VoidrvDisplayHandle handle, uint32_t index,
                                         const VoidrvDisplayMode* mode);

/* Read the current display table. */
bool                VoidrvDisplayList(VoidrvDisplayHandle handle, VoidrvDisplayState* state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VOIDRV_H */
