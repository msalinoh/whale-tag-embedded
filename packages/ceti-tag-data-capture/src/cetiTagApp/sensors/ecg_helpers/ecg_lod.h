//-----------------------------------------------------------------------------
// Project:      CETI Tag Electronics
// Version:      Refer to _versioning.h
// Copyright:    Cummings Electronics Labs, Harvard University Wood Lab, MIT CSAIL
// Contributors: Joseph DelPreto [TODO: Add other contributors here]
// Description:  Interfacing with the PCA9674 GPIO expander
//-----------------------------------------------------------------------------

#ifndef __CETI_WHALE_TAG_HAL_ECG_H__
#define __CETI_WHALE_TAG_HAL_ECG_H__

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
#include "../../utils/error.h"  // for WTResult
#include <stdint.h>             // for uint8_t

// ------------------------------------------
// Definitions/Configuration
// ------------------------------------------

//-----------------------------------------------------------------------------
// Methods
//-----------------------------------------------------------------------------

WTResult init_ecg_leadsOff();
WTResult ecg_read_leadsOff(int* leadsOff_p, int* leadsOff_n);
WTResult ecg_read_leadsOff_p(int* leadsOff_p);
WTResult ecg_read_leadsOff_n(int* leadsOff_n);

#endif // __CETI_WHALE_TAG_HAL_ECG_H__




