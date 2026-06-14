/*
 * VoidDisplay - EDID generation.
 *
 * Produces a valid 128-byte EDID 1.4 base block branded "VVD" / "VoidDisplay".
 * The advertised mode list is supplied separately through the IddCx
 * ParseMonitorDescription / QueryTargetModes callbacks, so this block only has
 * to be a structurally valid, identity-carrying EDID. The serial argument keeps
 * each monitor's EDID distinct so the OS does not collapse multiple virtual
 * monitors into one.
 */

#pragma once

#include <windows.h>

#define VOIDDISPLAY_EDID_SIZE 128
#define VOIDDISPLAY_EDID_MAX  256   // base block + one CTA extension

void VoidBuildEdid(UINT8 out[VOIDDISPLAY_EDID_SIZE], UINT32 serial);

/*
 * Load an operator-supplied EDID from C:\ProgramData\voidrv\display.edid (a dumped
 * real-monitor EDID). Returns the byte count (128 or 256) on success, or 0 if the
 * file is absent/invalid (caller falls back to VoidBuildEdid). Validates size,
 * header and checksum(s). When patchSerial, stamps a per-slot serial into bytes
 * 12-15 and fixes the base-block checksum so multiple monitors are not identical.
 */
UINT32 VoidLoadCustomEdid(UINT8 out[VOIDDISPLAY_EDID_MAX], UINT32 serial, bool patchSerial);
