// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include "HackIOSUi.h"
#include "DiagnosticScreen.h"
EFI_STATUS EFIAPI DarwinPreflightEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable);
STATIC EFI_STATUS
StartEmbedded (EFI_HANDLE ImageHandle, CONST EFI_GUID *Guid)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FileNode;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_HANDLE Child = NULL;
  EFI_STATUS Status;
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) return Status;
  FileNode.Header.Type = MEDIA_DEVICE_PATH;
  FileNode.Header.SubType = MEDIA_PIWG_FW_FILE_DP;
  SetDevicePathNodeLength (&FileNode.Header, sizeof (FileNode));
  CopyGuid (&FileNode.FvFileName, Guid);
  Path = AppendDevicePathNode (DevicePathFromHandle (Loaded->DeviceHandle), (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (Path == NULL) return EFI_OUT_OF_RESOURCES;
  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Child);
  FreePool (Path);
  if (EFI_ERROR (Status)) return Status;
  Status = gBS->StartImage (Child, NULL, NULL);
  gBS->UnloadImage (Child);
  return Status;
}
STATIC EFI_STATUS
ReadKey (EFI_INPUT_KEY *Key)
{
  EFI_STATUS Status;
  UINTN Index;
  Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  if (EFI_ERROR (Status)) return Status;
  return gST->ConIn->ReadKeyStroke (gST->ConIn, Key);
}
STATIC EFI_STATUS
DrawHackIOS (UINTN Selected)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_STATUS Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || Gop->Mode == NULL || Gop->Mode->Info == NULL) return EFI_UNSUPPORTED;
  UINT32 W = Gop->Mode->Info->HorizontalResolution;
  UINT32 H = Gop->Mode->Info->VerticalResolution;
  if (W < 320 || W > 4096 || H < W || H > 8192) return EFI_UNSUPPORTED;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels = AllocatePool ((UINTN)W * H * sizeof (*Pixels));
  if (Pixels == NULL) return EFI_OUT_OF_RESOURCES;
  HxCanvas Canvas = {(unsigned int *)Pixels, (int)W, (int)H, (int)W};
  hx_render (&Canvas, (int)Selected, 0);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  Status = Gop->Blt (Gop, Pixels, EfiBltBufferToVideo, 0, 0, 0, 0, W, H, 0);
  FreePool (Pixels);
  return Status;
}
EFI_STATUS EFIAPI
DarwinMenuEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  UINTN Selected = 0, Index;
  STATIC CONST EFI_GUID LoaderGuid = {0x8148efc2,0xc15b,0x4518,{0x94,0x78,0xc9,0x8f,0x6d,0x3c,0x72,0x15}};
  CONST CHAR8 *Items[] = {"Boot iOS (USB loader)", "Diagnostics", "USB Storage", "Restart Android"};
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  DrawHackIOS (Selected);
  Status = StartEmbedded (ImageHandle, &LoaderGuid);
  if (EFI_ERROR (Status)) {
    DiagnosticScreenStart ();
    DiagnosticLine ("HackIOS UEFI: automatic USB loader returned %r", Status);
  }
  for (;;) {
    Status = DrawHackIOS (Selected);
    if (EFI_ERROR (Status)) {
      Status = DiagnosticScreenStart ();
      if (EFI_ERROR (Status)) return Status;
      DiagnosticLine ("HackIOS UEFI -- OnePlus Ace 6T");
      DiagnosticLine ("Volume: select / Power: open");
      for (Index = 0; Index < 4; Index++)
        DiagnosticLine ("%a %a", Index == Selected ? ">" : " ", Items[Index]);
    }
    for (;;) {
      Status = ReadKey (&Key);
      if (EFI_ERROR (Status)) continue;
      if (Key.ScanCode == SCAN_UP) { Selected = (Selected + 3) % 4; break; }
      if (Key.ScanCode == SCAN_DOWN) { Selected = (Selected + 1) % 4; break; }
      if (Key.ScanCode != SCAN_ESC && Key.UnicodeChar != CHAR_CARRIAGE_RETURN) continue;
      if (Selected == 0) Status = StartEmbedded (ImageHandle, &LoaderGuid);
      else if (Selected == 1) Status = DarwinPreflightEntry (ImageHandle, SystemTable);
      else if (Selected == 2) Status = StartEmbedded (ImageHandle, FixedPcdGetPtr (PcdMassStorageFile));
      else {
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        Status = EFI_DEVICE_ERROR;
      }
      if (EFI_ERROR (Status)) {
        DiagnosticScreenStart ();
        DiagnosticLine ("HackIOS UEFI: application returned %r", Status);
        DiagnosticLine ("Press a button to return.");
        ReadKey (&Key);
      }
      break;
    }
  }
}
