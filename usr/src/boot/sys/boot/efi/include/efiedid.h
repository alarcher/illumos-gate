/*
  EDID Active/Discoverd/Override Protocol from the UEFI 2.0 specification.

  Placed on the video output device child handle that is actively displaying output.

  Copyright (c) 2006 - 2012, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef __EFIEDID_H__
#define	__EFIEDID_H__

#define EFI_EDID_ACTIVE_PROTOCOL_GUID \
  { \
    0xbd8c1056, 0x9f36, 0x44ec, {0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } \
  }

typedef struct {
  UINT32   SizeOfEdid;
  UINT8    *Edid;
} EFI_EDID_ACTIVE_PROTOCOL;

#define EFI_EDID_DISCOVERED_PROTOCOL_GUID \
  { \
    0x1c0c34f6, 0xd380, 0x41fa, {0xa0, 0x49, 0x8a, 0xd0, 0x6c, 0x1a, 0x66, 0xaa } \
  }

typedef struct {
  UINT32   SizeOfEdid;
  UINT8    *Edid;
} EFI_EDID_DISCOVERED_PROTOCOL;

#define EFI_EDID_OVERRIDE_PROTOCOL_GUID \
  { \
    0x48ecb431, 0xfb72, 0x45c0, {0xa9, 0x22, 0xf4, 0x58, 0xfe, 0x4, 0xb, 0xd5 } \
  }

typedef struct _EFI_EDID_OVERRIDE_PROTOCOL EFI_EDID_OVERRIDE_PROTOCOL;

#define	EFI_EDID_OVERRIDE_DONT_OVERRIDE   0x01
#define	EFI_EDID_OVERRIDE_ENABLE_HOT_PLUG 0x02

typedef
EFI_STATUS
(EFIAPI *EFI_EDID_OVERRIDE_PROTOCOL_GET_EDID)(
  IN  EFI_EDID_OVERRIDE_PROTOCOL	*This,
  IN  EFI_HANDLE			*ChildHandle,
  OUT UINT32				*Attributes,
  IN OUT UINTN				*EdidSize,
  IN OUT UINT8				**Edid
  );

struct _EFI_EDID_OVERRIDE_PROTOCOL {
  EFI_EDID_OVERRIDE_PROTOCOL_GET_EDID   GetEdid;
};

#endif	/* __EFIEDID_H__ */
