##
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = macan
  PLATFORM_GUID                  = 49301A81-23FB-4BDB-851D-1B4FBE643E04
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/macanPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = macanPkg/macan.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0



!include MolokaiPkg/MolokaiPkg.dsc.inc

[BuildOptions]
  *_*_*_CC_FLAGS = -DMACAN_DARWIN_PROJECT=1

[PcdsFixedAtBuild]
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString|L"1.0"
  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVendor|L"HackIOS UEFI"
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0xAC, 0xC4, 0x34, 0x86, 0x2D, 0x43, 0x57, 0x4D, 0x97, 0x14, 0x6C, 0x02, 0xD4, 0xF5, 0xD9, 0xBD }
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"OnePlus"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Ace 6T Darwin"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"PLR110"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"PLR110"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"24855"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1272
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2800
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"


[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|macanPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|macanPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  macanPkg/Applications/XnuRamLoader/XnuRamLoader.inf
  macanPkg/Applications/DarwinMenu/DarwinMenu.inf
  #
  # ACPI Tables
  #
  #macan/AcpiTables.inf
