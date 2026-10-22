// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/Fdt.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/GlobalVariable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/FdtLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include "DiagnosticScreen.h"
#define Line DiagnosticLine
#define FS_PER_PAGE 5
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC UINTN mRow;
STATIC UINTN mLeft;
STATIC UINTN mTop;
STATIC UINTN mLineHeight;
VOID
Line (IN CONST CHAR8 *Format, ...)
{
  CHAR8 Text[128];
  VA_LIST Args;
  UINTN Y;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL White = {0xff, 0xff, 0xff, 0};
  VA_START (Args, Format);
  AsciiVSPrint (Text, sizeof (Text), Format, Args);
  VA_END (Args);
  Y = mTop + mRow++ * mLineHeight;
  if (Text[0] == '\0') {
    return;
  }
  PrintXY (mLeft, Y, &White, NULL, L"%a", Text);
}
EFI_STATUS
DiagnosticScreenStart (VOID)
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0, 0, 0, 0};
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
  if (EFI_ERROR (Status) || mGop->Mode == NULL || mGop->Mode->Info == NULL) {
    return EFI_UNSUPPORTED;
  }
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  mLeft = 24;
  mLineHeight = 32;
  mTop = mGop->Mode->Info->VerticalResolution / 4;
  mRow = 0;
  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {}
  return mGop->Blt (mGop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
                   mGop->Mode->Info->HorizontalResolution, mGop->Mode->Info->VerticalResolution, 0);
}
STATIC VOID
MemorySummary (VOID)
{
  EFI_STATUS Status;
  EFI_MEMORY_DESCRIPTOR *Map = NULL;
  EFI_MEMORY_DESCRIPTOR *Entry;
  UINTN Size = 0, Capacity, Key, Stride = 0, Index, Attempts;
  UINT32 Version;
  UINT64 FreePages = 0;
  Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL || Stride == 0) {
    Line ("Memory map: %r", Status);
    return;
  }
  for (Attempts = 0; Attempts < 3; Attempts++) {
    Capacity = Size + 8 * Stride;
    Map = AllocatePool (Capacity);
    if (Map == NULL) {
      Line ("Memory map: allocation failed");
      return;
    }
    Size = Capacity;
    Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      break;
    }
    FreePool (Map);
    Map = NULL;
  }
  if (!EFI_ERROR (Status) && Stride >= sizeof (*Map)) {
    for (Index = 0; Index < Size / Stride; Index++) {
      Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * Stride);
      if (Entry->Type == EfiConventionalMemory) {
        FreePages += Entry->NumberOfPages;
      }
    }
    Line ("Memory: %u descriptors, %Lu MiB conventional", Size / Stride, FreePages / 256);
  } else {
    Line ("Memory map: %r", Status);
  }
  if (Map != NULL) {
    FreePool (Map);
  }
}
STATIC VOID
Summary (IN UINTN FsCount)
{
  EFI_STATUS Status;
  VOID *Table = NULL;
  CONST CHAR8 *Model;
  INT32 Length;
  EFI_HANDLE *Handles = NULL;
  UINTN Count = 0, Size;
  UINT8 SecureBoot = 0;
  Line ("READ-ONLY: no disk or firmware-variable writes");
  Line ("Firmware: %s / UEFI %x", gST->FirmwareVendor, gST->Hdr.Revision);
  Line ("GOP: %ux%u, pixel format %u", mGop->Mode->Info->HorizontalResolution,
        mGop->Mode->Info->VerticalResolution, mGop->Mode->Info->PixelFormat);
  Line ("Framebuffer: 0x%Lx, %Lu bytes", (UINT64)mGop->Mode->FrameBufferBase,
        (UINT64)mGop->Mode->FrameBufferSize);
  Size = sizeof (SecureBoot);
  Status = gRT->GetVariable (L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &Size, &SecureBoot);
  if (!EFI_ERROR (Status)) {
    Line ("SecureBoot: %u", SecureBoot);
  } else {
    Line ("SecureBoot variable: %r", Status);
  }
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Table);
  if (!EFI_ERROR (Status) && Table != NULL && FdtCheckHeader (Table) == 0) {
    Line ("Firmware FDT: present, %u bytes", FdtTotalSize (Table));
    Model = FdtGetProp (Table, 0, "model", &Length);
    if (Model != NULL && Length > 0 && AsciiStrnLenS (Model, (UINTN)Length) < (UINTN)Length) {
      Line ("DT model: %a", Model);
    }
  } else {
    Line ("Firmware FDT: absent or invalid (%r)", Status);
  }
  Table = NULL;
  Status = EfiGetSystemConfigurationTable (&gEfiAcpi20TableGuid, &Table);
  Line ("ACPI 2.0: %a", !EFI_ERROR (Status) && Table != NULL ? "present" : "absent");
  MemorySummary ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  Line ("Block IO: %u handles (%r)", Count, Status);
  if (Handles != NULL) {
    FreePool (Handles);
  }
  Line ("Simple FS: %u handles", FsCount);
  Line ("Volume Up: next page; Volume Down/Power: exit");
}
#define READ_SYSREG(Name) ({ UINT64 Value; __asm__ volatile ("mrs %0, " #Name : "=r" (Value)); Value; })
STATIC VOID
DarwinCpuSummary (VOID)
{
  UINT64 Mmfr0 = READ_SYSREG (ID_AA64MMFR0_EL1);
  UINT64 Gran16 = (Mmfr0 >> 20) & 15;
  Line ("DARWIN / XNU PREFLIGHT -- actual UEFI registers");
  Line ("CurrentEL: %Lu", READ_SYSREG (CurrentEL) >> 2);
  Line ("MIDR: %016Lx", READ_SYSREG (MIDR_EL1));
  Line ("MPIDR: %016Lx", READ_SYSREG (MPIDR_EL1));
  Line ("MMFR0: %016Lx", Mmfr0);
  Line ("MMFR1: %016Lx", READ_SYSREG (ID_AA64MMFR1_EL1));
  Line ("MMFR2: %016Lx", READ_SYSREG (ID_AA64MMFR2_EL1));
  Line ("PFR0: %016Lx", READ_SYSREG (ID_AA64PFR0_EL1));
  Line ("CNTFRQ: %Lu Hz", READ_SYSREG (CNTFRQ_EL0));
  Line ("TGran16: %Lx (%a)", Gran16,
        Gran16 == 0 ? "16K unsupported" :
        (Gran16 == 1 || Gran16 == 2) ? "16K supported" : "unknown encoding");
  Line ("GOP stride: %u pixels", mGop->Mode->Info->PixelsPerScanLine);
  Line ("Read-only probe; XNU is NOT started.");
  Line ("Photograph this page for the Ace 6T port.");
  Line ("Volume Up: next page; Volume Down/Power: exit");
}
STATIC VOID
FileSystem (IN EFI_HANDLE Handle, IN UINTN Index)
{
  EFI_STATUS Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_SYSTEM_INFO *Info;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  CHAR16 *Text;
  UINTN Size = 0;
  Status = gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) {
    Line ("FS %u: %r", Index, Status);
    return;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) {
    Line ("FS %u: OpenVolume %r", Index, Status);
    return;
  }
  Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL && Size >= SIZE_OF_EFI_FILE_SYSTEM_INFO && Size <= 65536) {
    Info = AllocateZeroPool (Size);
    if (Info != NULL) {
      Status = Root->GetInfo (Root, &gEfiFileSystemInfoGuid, &Size, Info);
      if (!EFI_ERROR (Status)) {
        Line ("FS %u: %Lu MiB, free %Lu MiB, read-only %u", Index,
              Info->VolumeSize / 1048576, Info->FreeSpace / 1048576, Info->ReadOnly);
        Line ("Label: %s", Info->VolumeLabel);
      } else {
        Line ("FS %u: GetInfo %r", Index, Status);
      }
      FreePool (Info);
    } else {
      Line ("FS %u: allocation failed", Index);
    }
  } else {
    Line ("FS %u: GetInfo %r", Index, Status);
  }
  Root->Close (Root);
  Path = DevicePathFromHandle (Handle);
  Text = Path != NULL ? ConvertDevicePathToText (Path, FALSE, FALSE) : NULL;
  Line ("Path: %s", Text != NULL ? Text : L"unavailable");
  if (Text != NULL) {
    FreePool (Text);
  }
  Line ("");
}
EFI_STATUS
EFIAPI
DarwinPreflightEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles = NULL;
  EFI_INPUT_KEY Key;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black = {0, 0, 0, 0};
  UINTN Count = 0, Page = 0, Pages, Index, EventIndex;
  Status = DiagnosticScreenStart ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    Count = 0;
  }
  Pages = 2 + (Count + FS_PER_PAGE - 1) / FS_PER_PAGE;
  for (;;) {
    Status = mGop->Blt (mGop, &Black, EfiBltVideoFill, 0, 0, 0, 0,
                       mGop->Mode->Info->HorizontalResolution, mGop->Mode->Info->VerticalResolution, 0);
    if (EFI_ERROR (Status)) {
      break;
    }
    mRow = 0;
    Line ("ACE 6T DARWIN PREFLIGHT -- page %u/%u", Page + 1, Pages);
    Line ("");
    if (Page == 0) {
      Summary (Count);
    } else if (Page == 1) {
      DarwinCpuSummary ();
    } else {
      for (Index = (Page - 2) * FS_PER_PAGE; Index < Count && Index < (Page - 1) * FS_PER_PAGE; Index++) {
        FileSystem (Handles[Index], Index);
      }
      Line ("Volume Up: next page; Volume Down/Power: exit");
    }
    Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
    if (EFI_ERROR (Status)) {
      break;
    }
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      continue;
    }
    if (Key.ScanCode == SCAN_UP) {
      Page = (Page + 1) % Pages;
    } else if (Key.ScanCode == SCAN_DOWN || Key.ScanCode == SCAN_ESC || Key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
      Status = EFI_SUCCESS;
      break;
    }
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }
  return Status;
}
