// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef MACAN_DIAGNOSTIC_SCREEN_H
#define MACAN_DIAGNOSTIC_SCREEN_H 
#include <Uefi.h>
EFI_STATUS DiagnosticScreenStart (VOID);
VOID DiagnosticLine (IN CONST CHAR8 *Format, ...);
#endif
