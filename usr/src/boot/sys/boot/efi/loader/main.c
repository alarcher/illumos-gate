/*-
 * Copyright (c) 2008-2010 Rui Paulo
 * Copyright (c) 2006 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/reboot.h>
#include <sys/boot.h>
#include <stand.h>
#include <string.h>
#include <setjmp.h>

#include <efi.h>
#include <efilib.h>
#include <efigpt.h>

#include <uuid.h>

#include <bootstrap.h>
#include <smbios.h>

#ifdef EFI_ZFS_BOOT
#include <libzfs.h>
#endif

#include "loader_efi.h"

extern char bootprog_name[];
extern char bootprog_rev[];
extern char bootprog_date[];
extern char bootprog_maker[];

struct arch_switch archsw;	/* MI/MD interface boundary */

EFI_GUID acpi = ACPI_TABLE_GUID;
EFI_GUID acpi20 = ACPI_20_TABLE_GUID;
EFI_GUID devid = DEVICE_PATH_PROTOCOL;
EFI_GUID imgid = LOADED_IMAGE_PROTOCOL;
EFI_GUID mps = MPS_TABLE_GUID;
EFI_GUID netid = EFI_SIMPLE_NETWORK_PROTOCOL;
EFI_GUID smbios = SMBIOS_TABLE_GUID;
EFI_GUID smbios3 = SMBIOS3_TABLE_GUID;
EFI_GUID dxe = DXE_SERVICES_TABLE_GUID;
EFI_GUID hoblist = HOB_LIST_TABLE_GUID;
EFI_GUID memtype = MEMORY_TYPE_INFORMATION_TABLE_GUID;
EFI_GUID debugimg = DEBUG_IMAGE_INFO_TABLE_GUID;
EFI_GUID fdtdtb = FDT_TABLE_GUID;
EFI_GUID inputid = SIMPLE_TEXT_INPUT_PROTOCOL;

extern void efi_getsmap(void);
#ifdef EFI_ZFS_BOOT
static void efi_zfs_probe(void);
#endif

/*
 * cpy8to16 copies a traditional C string into a CHAR16 string and
 * 0 terminates it. len is the size of *dst in bytes.
 */
static void
cpy8to16(const char *src, CHAR16 *dst, size_t len)
{
	len <<= 1;		/* Assume CHAR16 is 2 bytes */
	while (len > 0 && *src) {
		*dst++ = *src++;
		len--;
	}
	*dst++ = (CHAR16)0;
}

static void
cpy16to8(const CHAR16 *src, char *dst, size_t len)
{
	size_t i;

	for (i = 0; i < len && src[i]; i++)
		dst[i] = (char)src[i];
	if (i < len)
		dst[i] = '\0';
}

static int
has_keyboard(void)
{
	EFI_STATUS status;
	EFI_DEVICE_PATH *path;
	EFI_HANDLE *hin, *hin_end, *walker;
	UINTN sz;
	int retval = 0;
	
	/*
	 * Find all the handles that support the SIMPLE_TEXT_INPUT_PROTOCOL and
	 * do the typical dance to get the right sized buffer.
	 */
	sz = 0;
	hin = NULL;
	status = BS->LocateHandle(ByProtocol, &inputid, 0, &sz, 0);
	if (status == EFI_BUFFER_TOO_SMALL) {
		hin = (EFI_HANDLE *)malloc(sz);
		status = BS->LocateHandle(ByProtocol, &inputid, 0, &sz,
		    hin);
		if (EFI_ERROR(status))
			free(hin);
	}
	if (EFI_ERROR(status))
		return retval;

	/*
	 * Look at each of the handles. If it supports the device path protocol,
	 * use it to get the device path for this handle. Then see if that
	 * device path matches either the USB device path for keyboards or the
	 * legacy device path for keyboards.
	 */
	hin_end = &hin[sz / sizeof(*hin)];
	for (walker = hin; walker < hin_end; walker++) {
		status = BS->HandleProtocol(*walker, &devid, (VOID **)&path);
		if (EFI_ERROR(status))
			continue;

		while (!IsDevicePathEnd(path)) {
			/*
			 * Check for the ACPI keyboard node. All PNP3xx nodes
			 * are keyboards of different flavors. Note: It is
			 * unclear of there's always a keyboard node when
			 * there's a keyboard controller, or if there's only one
			 * when a keyboard is detected at boot.
			 */
			if (DevicePathType(path) == ACPI_DEVICE_PATH &&
			    (DevicePathSubType(path) == ACPI_DP ||
				DevicePathSubType(path) == ACPI_EXTENDED_DP)) {
				ACPI_HID_DEVICE_PATH  *acpi;

				acpi = (ACPI_HID_DEVICE_PATH *)(void *)path;
				if ((EISA_ID_TO_NUM(acpi->HID) & 0xff00) == 0x300 &&
				    (acpi->HID & 0xffff) == PNP_EISA_ID_CONST) {
					BS->CloseProtocol(*walker, &devid,
					    IH, NULL);
					retval = 1;
					goto out;
				}
			/*
			 * Check for USB keyboard node, if present. Unlike a
			 * PS/2 keyboard, these definitely only appear when
			 * connected to the system.
			 */
			} else if (DevicePathType(path) == MESSAGING_DEVICE_PATH &&
			    DevicePathSubType(path) == MSG_USB_CLASS_DP) {
				USB_CLASS_DEVICE_PATH *usb;
			       
				usb = (USB_CLASS_DEVICE_PATH *)(void *)path;
				if (usb->DeviceClass == 3 && /* HID */
				    usb->DeviceSubClass == 1 && /* Boot devices */
				    usb->DeviceProtocol == 1) { /* Boot keyboards */
					BS->CloseProtocol(*walker, &devid,
					    IH, NULL);
					retval = 1;
					goto out;
				}
			}
			path = NextDevicePathNode(path);
		}
		BS->CloseProtocol(*walker, &devid, IH, NULL);
	}
out:
	free(hin);
	return retval;
}

static int
find_currdev(EFI_LOADED_IMAGE *img, struct devsw **dev, int *unit,
    uint64_t *extra)
{
	EFI_DEVICE_PATH *devpath, *copy;
	EFI_HANDLE h;

	/*
	 * Try the device handle from our loaded image first.  If that
	 * fails, use the device path from the loaded image and see if
	 * any of the nodes in that path match one of the enumerated
	 * handles.
	 */
	if (efi_handle_lookup(img->DeviceHandle, dev, unit, extra) == 0)
		return (0);

	copy = NULL;
	devpath = efi_lookup_image_devpath(IH);
	while (devpath != NULL) {
		h = efi_devpath_handle(devpath);
		if (h == NULL)
			break;
		if (efi_handle_lookup(h, dev, unit, extra) == 0) {
			if (copy != NULL)
				free(copy);
			return (0);
		}

		if (copy != NULL)
			free(copy);

		devpath = efi_lookup_devpath(h);
		if (devpath != NULL) {
			copy = efi_devpath_trim(devpath);
			devpath = copy;
		}
	}
	return (ENOENT);
}

EFI_STATUS
main(int argc, CHAR16 *argv[])
{
	char var[128];
	EFI_LOADED_IMAGE *img;
	EFI_GUID *guid;
	int i, j, vargood, unit, howto;
	struct devsw *dev;
	uint64_t pool_guid;
	void *ptr;
	UINTN k;
	int has_kbd;

	archsw.arch_autoload = efi_autoload;
	archsw.arch_getdev = efi_getdev;
	archsw.arch_copyin = efi_copyin;
	archsw.arch_copyout = efi_copyout;
	archsw.arch_readin = efi_readin;
	archsw.arch_loadaddr = efi_loadaddr;
	archsw.arch_free_loadaddr = efi_free_loadaddr;
#ifdef EFI_ZFS_BOOT
	/* Note this needs to be set before ZFS init. */
	archsw.arch_zfs_probe = efi_zfs_probe;
#endif

	/* Init the time source */
	efi_time_init();

	has_kbd = has_keyboard();

	/*
	 * XXX Chicken-and-egg problem; we want to have console output
	 * early, but some console attributes may depend on reading from
	 * eg. the boot device, which we can't do yet.  We can use
	 * printf() etc. once this is done.
	 */
	cons_probe();
	efi_getsmap();

	/*
	 * Initialise the block cache. Set the upper limit.
	 */
	bcache_init(32768, 512);

	/*
	 * Parse the args to set the console settings, etc
	 * boot1.efi passes these in, if it can read /boot.config or /boot/config
	 * or iPXE may be setup to pass these in.
	 *
	 * Loop through the args, and for each one that contains an '=' that is
	 * not the first character, add it to the environment.  This allows
	 * loader and kernel env vars to be passed on the command line.  Convert
	 * args from UCS-2 to ASCII (16 to 8 bit) as they are copied.
	 */
	howto = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			for (j = 1; argv[i][j] != 0; j++) {
				int ch;

				ch = argv[i][j];
				switch (ch) {
				case 'a':
					howto |= RB_ASKNAME;
					break;
				case 'd':
					howto |= RB_KDB;
					break;
				case 'D':
					howto |= RB_MULTIPLE;
					break;
				case 'h':
					howto |= RB_SERIAL;
					break;
				case 'm':
					howto |= RB_MUTE;
					break;
				case 'p':
					howto |= RB_PAUSE;
					break;
				case 'P':
					if (!has_kbd)
						howto |= RB_SERIAL | RB_MULTIPLE;
					break;
				case 'r':
					howto |= RB_DFLTROOT;
					break;
				case 's':
					howto |= RB_SINGLE;
					break;
				case 'S':
					if (argv[i][j + 1] == 0) {
						if (i + 1 == argc) {
							setenv("comconsole_speed", "115200", 1);
						} else {
							cpy16to8(&argv[i + 1][0], var,
							    sizeof(var));
							setenv("comconsole_speedspeed", var, 1);
						}
						i++;
						break;
					} else {
						cpy16to8(&argv[i][j + 1], var,
						    sizeof(var));
						setenv("comconsole_speed", var, 1);
						break;
					}
				case 'v':
					howto |= RB_VERBOSE;
					break;
				}
			}
		} else {
			vargood = 0;
			for (j = 0; argv[i][j] != 0; j++) {
				if (j == sizeof(var)) {
					vargood = 0;
					break;
				}
				if (j > 0 && argv[i][j] == '=')
					vargood = 1;
				var[j] = (char)argv[i][j];
			}
			if (vargood) {
				var[j] = 0;
				putenv(var);
			}
		}
	}
	for (i = 0; howto_names[i].ev != NULL; i++)
		if (howto & howto_names[i].mask)
			setenv(howto_names[i].ev, "YES", 1);
	if (howto & RB_MULTIPLE) {
		if (howto & RB_SERIAL)
			setenv("console", "ttya text" , 1);
		else
			setenv("console", "text ttya" , 1);
	} else if (howto & RB_SERIAL) {
		setenv("console", "ttya" , 1);
	}

	/*
	 * March through the device switch probing for things.
	 */
	for (i = 0; devsw[i] != NULL; i++)
		if (devsw[i]->dv_init != NULL)
			(devsw[i]->dv_init)();

	/* Get our loaded image protocol interface structure. */
	BS->HandleProtocol(IH, &imgid, (VOID**)&img);

	printf("Command line arguments:");
	for (i = 0; i < argc; i++)
		printf(" %S", argv[i]);
	printf("\n");

	printf("Image base: 0x%lx\n", (u_long)img->ImageBase);
	printf("EFI version: %d.%02d\n", ST->Hdr.Revision >> 16,
	    ST->Hdr.Revision & 0xffff);
	printf("EFI Firmware: %S (rev %d.%02d)\n", ST->FirmwareVendor,
	    ST->FirmwareRevision >> 16, ST->FirmwareRevision & 0xffff);

	printf("\n");
	printf("%s, Revision %s\n", bootprog_name, bootprog_rev);
	printf("(%s, %s)\n", bootprog_maker, bootprog_date);

	/*
	 * Disable the watchdog timer. By default the boot manager sets
	 * the timer to 5 minutes before invoking a boot option. If we
	 * want to return to the boot manager, we have to disable the
	 * watchdog timer and since we're an interactive program, we don't
	 * want to wait until the user types "quit". The timer may have
	 * fired by then. We don't care if this fails. It does not prevent
	 * normal functioning in any way...
	 */
	BS->SetWatchdogTimer(0, 0, 0, NULL);

	if (find_currdev(img, &dev, &unit, &pool_guid) != 0)
		return (EFI_NOT_FOUND);

	switch (dev->dv_type) {
#ifdef EFI_ZFS_BOOT
	case DEVT_ZFS: {
		struct zfs_devdesc currdev;

		currdev.d_dev = dev;
		currdev.d_unit = unit;
		currdev.d_type = currdev.d_dev->dv_type;
		currdev.d_opendata = NULL;
		currdev.pool_guid = pool_guid;
		currdev.root_guid = 0;
		env_setenv("currdev", EV_VOLATILE, efi_fmtdev(&currdev),
			   efi_setcurrdev, env_nounset);
		env_setenv("loaddev", EV_VOLATILE, efi_fmtdev(&currdev), env_noset,
			   env_nounset);
#ifdef __FreeBSD__
		init_zfs_bootenv(zfs_fmtdev(&currdev));
#endif
		break;
	}
#endif
	default: {
		struct devdesc currdev;

		currdev.d_dev = dev;
		currdev.d_unit = unit;
		currdev.d_opendata = NULL;
		currdev.d_type = currdev.d_dev->dv_type;
		env_setenv("currdev", EV_VOLATILE, efi_fmtdev(&currdev),
			   efi_setcurrdev, env_nounset);
		env_setenv("loaddev", EV_VOLATILE, efi_fmtdev(&currdev), env_noset,
			   env_nounset);
		break;
	}
	}

	snprintf(var, sizeof(var), "%d.%02d", ST->Hdr.Revision >> 16,
	    ST->Hdr.Revision & 0xffff);
	env_setenv("efi-version", EV_VOLATILE, var, env_noset, env_nounset);

	/* set ISADIR */
	bi_isadir();

	acpi_detect();

	if ((ptr = efi_get_table(&smbios3)) == NULL)
		ptr = efi_get_table(&smbios);
	smbios_detect(ptr);

	interact(NULL);			/* doesn't return */

	return (EFI_SUCCESS);		/* keep compiler happy */
}

/* XXX move to lib stand ? */
static int
wcscmp(CHAR16 *a, CHAR16 *b)
{

	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return *a - *b;
}


COMMAND_SET(reboot, "reboot", "reboot the system", command_reboot);

static int
command_reboot(int argc __unused, char *argv[] __unused)
{
	int i;

	for (i = 0; devsw[i] != NULL; ++i)
		if (devsw[i]->dv_cleanup != NULL)
			(devsw[i]->dv_cleanup)();

	RS->ResetSystem(EfiResetCold, EFI_SUCCESS, 23,
	    (CHAR16 *)"Reboot from the loader");

	/* NOTREACHED */
	return (CMD_ERROR);
}

COMMAND_SET(memmap, "memmap", "print memory map", command_memmap);

static int
command_memmap(int argc __unused, char *argv[] __unused)
{
	UINTN sz;
	EFI_MEMORY_DESCRIPTOR *map, *p;
	UINTN key, dsz;
	UINT32 dver;
	EFI_STATUS status;
	int i, ndesc;
	int rv = 0;
	char line[80];
	static const char *types[] = {
	    "Reserved",
	    "LoaderCode",
	    "LoaderData",
	    "BootServicesCode",
	    "BootServicesData",
	    "RuntimeServicesCode",
	    "RuntimeServicesData",
	    "ConventionalMemory",
	    "UnusableMemory",
	    "ACPIReclaimMemory",
	    "ACPIMemoryNVS",
	    "MemoryMappedIO",
	    "MemoryMappedIOPortSpace",
	    "PalCode"
	};

	sz = 0;
	status = BS->GetMemoryMap(&sz, 0, &key, &dsz, &dver);
	if (status != EFI_BUFFER_TOO_SMALL) {
		printf("Can't determine memory map size\n");
		return (CMD_ERROR);
	}
	map = malloc(sz);
	status = BS->GetMemoryMap(&sz, map, &key, &dsz, &dver);
	if (EFI_ERROR(status)) {
		printf("Can't read memory map\n");
		return (CMD_ERROR);
	}

	ndesc = sz / dsz;
	snprintf(line, 80, "%23s %12s %12s %8s %4s\n",
	       "Type", "Physical", "Virtual", "#Pages", "Attr");
	pager_open();
	rv = pager_output(line);
	if (rv) {
		pager_close();
		return (CMD_OK);
	}

	for (i = 0, p = map; i < ndesc;
	     i++, p = NextMemoryDescriptor(p, dsz)) {
		snprintf(line, 80, "%23s %012lx %012lx %08lx ",
		    types[p->Type],
		    (unsigned long)p->PhysicalStart,
		    (unsigned long)p->VirtualStart,
		    (unsigned long)p->NumberOfPages);
		rv = pager_output(line);
		if (rv)
			break;

		if (p->Attribute & EFI_MEMORY_UC)
			printf("UC ");
		if (p->Attribute & EFI_MEMORY_WC)
			printf("WC ");
		if (p->Attribute & EFI_MEMORY_WT)
			printf("WT ");
		if (p->Attribute & EFI_MEMORY_WB)
			printf("WB ");
		if (p->Attribute & EFI_MEMORY_UCE)
			printf("UCE ");
		if (p->Attribute & EFI_MEMORY_WP)
			printf("WP ");
		if (p->Attribute & EFI_MEMORY_RP)
			printf("RP ");
		if (p->Attribute & EFI_MEMORY_XP)
			printf("XP ");
		rv = pager_output("\n");
		if (rv)
			break;
	}

	pager_close();
	return (CMD_OK);
}

COMMAND_SET(configuration, "configuration", "print configuration tables",
    command_configuration);

static const char *
guid_to_string(EFI_GUID *guid)
{
	static char buf[40];

	sprintf(buf, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
	    guid->Data1, guid->Data2, guid->Data3, guid->Data4[0],
	    guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4],
	    guid->Data4[5], guid->Data4[6], guid->Data4[7]);
	return (buf);
}

static int
command_configuration(int argc __unused, char *argv[] __unused)
{
	UINTN i;

	printf("NumberOfTableEntries=%lu\n",
		(unsigned long)ST->NumberOfTableEntries);
	for (i = 0; i < ST->NumberOfTableEntries; i++) {
		EFI_GUID *guid;

		printf("  ");
		guid = &ST->ConfigurationTable[i].VendorGuid;
		if (!memcmp(guid, &mps, sizeof(EFI_GUID)))
			printf("MPS Table");
		else if (!memcmp(guid, &acpi, sizeof(EFI_GUID)))
			printf("ACPI Table");
		else if (!memcmp(guid, &acpi20, sizeof(EFI_GUID)))
			printf("ACPI 2.0 Table");
		else if (!memcmp(guid, &smbios, sizeof(EFI_GUID)))
			printf("SMBIOS Table");
		else if (!memcmp(guid, &smbios3, sizeof(EFI_GUID)))
			printf("SMBIOS3 Table");
		else if (!memcmp(guid, &dxe, sizeof(EFI_GUID)))
			printf("DXE Table");
		else if (!memcmp(guid, &hoblist, sizeof(EFI_GUID)))
			printf("HOB List Table");
		else if (!memcmp(guid, &memtype, sizeof(EFI_GUID)))
			printf("Memory Type Information Table");
		else if (!memcmp(guid, &debugimg, sizeof(EFI_GUID)))
			printf("Debug Image Info Table");
		else if (!memcmp(guid, &fdtdtb, sizeof(EFI_GUID)))
			printf("FDT Table");
		else
			printf("Unknown Table (%s)", guid_to_string(guid));
		printf(" at %p\n", ST->ConfigurationTable[i].VendorTable);
	}

	return (CMD_OK);
}


COMMAND_SET(mode, "mode", "change or display EFI text modes", command_mode);

static int
command_mode(int argc, char *argv[])
{
	UINTN cols, rows;
	unsigned int mode;
	int i;
	char *cp;
	char rowenv[8];
	EFI_STATUS status;
	SIMPLE_TEXT_OUTPUT_INTERFACE *conout;
	extern void HO(void);

	conout = ST->ConOut;

	if (argc > 1) {
		mode = strtol(argv[1], &cp, 0);
		if (cp[0] != '\0') {
			printf("Invalid mode\n");
			return (CMD_ERROR);
		}
		status = conout->QueryMode(conout, mode, &cols, &rows);
		if (EFI_ERROR(status)) {
			printf("invalid mode %d\n", mode);
			return (CMD_ERROR);
		}
		status = conout->SetMode(conout, mode);
		if (EFI_ERROR(status)) {
			printf("couldn't set mode %d\n", mode);
			return (CMD_ERROR);
		}
		sprintf(rowenv, "%u", (unsigned)rows);
		setenv("LINES", rowenv, 1);
		sprintf(rowenv, "%u", (unsigned)cols);
		setenv("COLUMNS", rowenv, 1);
		HO();		/* set cursor */
		return (CMD_OK);
	}

	printf("Current mode: %d\n", conout->Mode->Mode);
	for (i = 0; i <= conout->Mode->MaxMode; i++) {
		status = conout->QueryMode(conout, i, &cols, &rows);
		if (EFI_ERROR(status))
			continue;
		printf("Mode %d: %u columns, %u rows\n", i, (unsigned)cols,
		    (unsigned)rows);
	}

	if (i != 0)
		printf("Select a mode with the command \"mode <number>\"\n");

	return (CMD_OK);
}


struct protocol_name {
	EFI_GUID guid;
	const char *name;
} proto_names[] = {
	{ DEVICE_PATH_PROTOCOL, "device path" },
	{ BLOCK_IO_PROTOCOL, "block io" },
	{ DISK_IO_PROTOCOL, "disk io" },
	{ EFI_DISK_INFO_PROTOCOL_GUID, "disk info" },
	{ SIMPLE_FILE_SYSTEM_PROTOCOL, "simple fs" },
	{ LOAD_FILE_PROTOCOL, "load file" },
	{ DEVICE_IO_PROTOCOL, "device io" },
	{ UNICODE_COLLATION_PROTOCOL, "unicode collation" },
	{ EFI_UNICODE_COLLATION2_PROTOCOL_GUID, "unicode collation2" },
	{ EFI_SIMPLE_NETWORK_PROTOCOL, "simple network" },
	{ SIMPLE_TEXT_OUTPUT_PROTOCOL, "simple text output" },
	{ SIMPLE_TEXT_INPUT_PROTOCOL, "simple text input" },
	{ EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID, "simple text ex input" },
	{ EFI_CONSOLE_CONTROL_PROTOCOL_GUID, "console control" },
	{ EFI_CONSOLE_IN_DEVICE_GUID, "stdin" },
	{ EFI_CONSOLE_OUT_DEVICE_GUID, "stdout" },
	{ EFI_STANDARD_ERROR_DEVICE_GUID, "stderr" },
	{ EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, "GOP" },
	{ EFI_UGA_DRAW_PROTOCOL_GUID, "UGA draw" },
	{ EFI_PXE_BASE_CODE_PROTOCOL, "PXE base code" },
	{ EFI_PXE_BASE_CODE_CALLBACK_PROTOCOL, "PXE base code callback" },
	{ SERIAL_IO_PROTOCOL, "serial io" },
	{ LOADED_IMAGE_PROTOCOL, "loaded image" },
	{ EFI_LOADED_IMAGE_DEVICE_PATH_PROTOCOL_GUID,
	    "loaded image device path" },
	{ EFI_ISA_IO_PROTOCOL_GUID, "ISA io" },
	{ EFI_IDE_CONTROLLER_INIT_PROTOCOL_GUID, "IDE controller init" },
	{ EFI_ISA_ACPI_PROTOCOL_GUID, "ISA ACPI" },
	{ EFI_PCI_IO_PROTOCOL_GUID, "PCI" },
	{ EFI_PCI_ROOT_IO_GUID, "PCI root" },
	{ EFI_PCI_ENUMERATION_COMPLETE_GUID, "PCI enumeration" },
	{ EFI_DRIVER_DIAGNOSTICS_PROTOCOL_GUID, "Driver diagnostics" },
	{ EFI_DRIVER_DIAGNOSTICS2_PROTOCOL_GUID, "Driver diagnostics2" },
	{ EFI_SIMPLE_POINTER_PROTOCOL_GUID, "simple pointer" },
	{ EFI_ABSOLUTE_POINTER_PROTOCOL_GUID, "absolute pointer" },
	{ EFI_VLAN_CONFIG_PROTOCOL_GUID, "VLAN config" },
	{ EFI_ARP_SERVICE_BINDING_PROTOCOL_GUID, "ARP service binding" },
	{ EFI_ARP_PROTOCOL_GUID, "ARP" },
	{ EFI_IP4_SERVICE_BINDING_PROTOCOL, "IPv4 service binding" },
	{ EFI_IP4_PROTOCOL, "IPv4" },
	{ EFI_IP4_CONFIG_PROTOCOL_GUID, "IPv4 config" },
	{ EFI_IP6_SERVICE_BINDING_PROTOCOL, "IPv6 service binding" },
	{ EFI_IP6_PROTOCOL, "IPv6" },
	{ EFI_IP6_CONFIG_PROTOCOL_GUID, "IPv6 config" },
	{ EFI_UDP4_PROTOCOL, "UDPv4" },
	{ EFI_UDP4_SERVICE_BINDING_PROTOCOL, "UDPv4 service binding" },
	{ EFI_UDP6_PROTOCOL, "UDPv6" },
	{ EFI_UDP6_SERVICE_BINDING_PROTOCOL, "UDPv6 service binding" },
	{ EFI_TCP4_PROTOCOL, "TCPv4" },
	{ EFI_TCP4_SERVICE_BINDING_PROTOCOL, "TCPv4 service binding" },
	{ EFI_TCP6_PROTOCOL, "TCPv6" },
	{ EFI_TCP6_SERVICE_BINDING_PROTOCOL, "TCPv6 service binding" },
	{ EFI_PART_TYPE_EFI_SYSTEM_PART_GUID, "EFI System partition" },
	{ EFI_PART_TYPE_LEGACY_MBR_GUID, "MBR legacy" },
	{ EFI_DEVICE_TREE_GUID, "device tree" },
	{ EFI_USB_IO_PROTOCOL_GUID, "USB io" },
	{ EFI_USB2_HC_PROTOCOL_GUID, "USB2 HC" },
	{ EFI_COMPONENT_NAME_PROTOCOL_GUID, "component name" },
	{ EFI_COMPONENT_NAME2_PROTOCOL_GUID, "component name2" },
	{ EFI_DRIVER_BINDING_PROTOCOL_GUID, "driver binding" },
	{ EFI_DRIVER_CONFIGURATION_PROTOCOL_GUID, "driver configuration" },
	{ EFI_DRIVER_CONFIGURATION2_PROTOCOL_GUID, "driver configuration2" },
	{ EFI_DECOMPRESS_PROTOCOL_GUID, "decompress" },
	{ EFI_EBC_INTERPRETER_PROTOCOL_GUID, "ebc interpreter" },
	{ EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL,
	    "network interface identifier" },
	{ EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL_31,
	    "network interface identifier_31" },
	{ EFI_MANAGED_NETWORK_SERVICE_BINDING_PROTOCOL_GUID,
	    "managed network service binding" },
	{ EFI_MANAGED_NETWORK_PROTOCOL_GUID, "managed network" },
	{ EFI_FORM_BROWSER2_PROTOCOL_GUID, "form browser" },
	{ EFI_HII_CONFIG_ROUTING_PROTOCOL_GUID, "HII config routing" },
	{ EFI_HII_DATABASE_PROTOCOL_GUID, "HII database" },
	{ EFI_HII_STRING_PROTOCOL_GUID, "HII string" },
	{ EFI_HII_IMAGE_PROTOCOL_GUID, "HII image" },
	{ EFI_HII_FONT_PROTOCOL_GUID, "HII font" },
	{ EFI_HII_CONFIGURATION_ACCESS_PROTOCOL_GUID, "HII config" },
	{ EFI_MTFTP4_SERVICE_BINDING_PROTOCOL_GUID, "MTFTP4 service binding" },
	{ EFI_MTFTP4_PROTOCOL_GUID, "MTFTP4" },
	{ EFI_MTFTP6_SERVICE_BINDING_PROTOCOL_GUID, "MTFTP6 service binding" },
	{ EFI_MTFTP6_PROTOCOL_GUID, "MTFTP6" },
	{ EFI_DHCP4_SERVICE_BINDING_PROTOCOL_GUID, "DHCP4 service binding" },
	{ EFI_DHCP4_PROTOCOL_GUID, "DHCP4" },
	{ EFI_DHCP6_SERVICE_BINDING_PROTOCOL_GUID, "DHCP6 service binding" },
	{ EFI_DHCP6_PROTOCOL_GUID, "DHCP6" },
	{ EFI_SCSI_IO_PROTOCOL_GUID, "SCSI io" },
	{ EFI_SCSI_PASS_THRU_PROTOCOL_GUID, "SCSI pass thru" },
	{ EFI_EXT_SCSI_PASS_THRU_PROTOCOL_GUID, "SCSI pass thru ext" },
	{ EFI_CAPSULE_ARCH_PROTOCOL_GUID, "Capsule arch" },
	{ EFI_MONOTONIC_COUNTER_ARCH_PROTOCOL_GUID, "monotonic counter arch" },
	{ EFI_REALTIME_CLOCK_ARCH_PROTOCOL_GUID, "realtime clock arch" },
	{ EFI_VARIABLE_ARCH_PROTOCOL_GUID, "variable arch" },
	{ EFI_VARIABLE_WRITE_ARCH_PROTOCOL_GUID, "variable write arch" },
	{ EFI_WATCHDOG_TIMER_ARCH_PROTOCOL_GUID, "watchdog timer arch" },
	{ EFI_MP_SERVICES_PROTOCOL_GUID, "MP services" },
	{ EFI_ACPI_SUPPORT_PROTOCOL_GUID, "ACPI support" },
	{ EFI_BDS_ARCH_PROTOCOL_GUID, "BDS arch" },
	{ EFI_METRONOME_ARCH_PROTOCOL_GUID, "metronome arch" },
	{ EFI_TIMER_ARCH_PROTOCOL_GUID, "timer arch" },
	{ EFI_DPC_PROTOCOL_GUID, "DPC" },
	{ EFI_PRINT2_PROTOCOL_GUID, "print2" },
	{ EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID, "device path to text" },
	{ EFI_RESET_ARCH_PROTOCOL_GUID, "reset arch" },
	{ EFI_CPU_ARCH_PROTOCOL_GUID, "CPU arch" },
	{ EFI_CPU_IO2_PROTOCOL_GUID, "CPU IO2" },
	{ EFI_LEGACY_8259_PROTOCOL_GUID, "Legacy 8259" },
	{ EFI_SECURITY_ARCH_PROTOCOL_GUID, "Security arch" },
	{ EFI_SECURITY2_ARCH_PROTOCOL_GUID, "Security2 arch" },
	{ EFI_RUNTIME_ARCH_PROTOCOL_GUID, "Runtime arch" },
	{ EFI_STATUS_CODE_RUNTIME_PROTOCOL_GUID, "status code runtime" },
	{ EFI_DATA_HUB_PROTOCOL_GUID, "data hub" },
	{ PCD_PROTOCOL_GUID, "PCD" },
	{ EFI_PCD_PROTOCOL_GUID, "EFI PCD" },
	{ EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL_GUID, "firmware volume block" },
	{ EFI_FIRMWARE_VOLUME2_PROTOCOL_GUID, "firmware volume2" },
	{ EFI_FIRMWARE_VOLUME_DISPATCH_PROTOCOL_GUID,
	    "firmware volume dispatch" },
	{ LZMA_COMPRESS_GUID, "lzma compress" },
	{ EFI_EDID_ACTIVE_PROTOCOL_GUID, "Active EDID" },
	{ EFI_EDID_DISCOVERED_PROTOCOL_GUID, "Discovered EDID" },
	{ { 0,0,0,{0,0,0,0,0,0,0,0} }, NULL }	/* must be last entry */
};

COMMAND_SET(lsefi, "lsefi", "list EFI handles", command_lsefi);

static int
command_lsefi(int argc __unused, char *argv[] __unused)
{
	EFI_HANDLE *buffer = NULL;
	EFI_HANDLE handle;
	UINTN bufsz = 0, i, j;
	EFI_STATUS status;
	int k, ret;

	status = BS->LocateHandle(AllHandles, NULL, NULL, &bufsz, buffer);
	if (status != EFI_BUFFER_TOO_SMALL) {
		snprintf(command_errbuf, sizeof (command_errbuf),
		    "unexpected error: %lld", (long long)status);
		return (CMD_ERROR);
	}
	if ((buffer = malloc(bufsz)) == NULL) {
		sprintf(command_errbuf, "out of memory");
		return (CMD_ERROR);
	}

	status = BS->LocateHandle(AllHandles, NULL, NULL, &bufsz, buffer);
	if (EFI_ERROR(status)) {
		free(buffer);
		snprintf(command_errbuf, sizeof (command_errbuf),
		    "LocateHandle() error: %lld", (long long)status);
		return (CMD_ERROR);
	}

	pager_open();
	for (i = 0; i < (bufsz / sizeof (EFI_HANDLE)); i++) {
		UINTN nproto = 0;
		EFI_GUID **protocols = NULL;

		handle = buffer[i];
		printf("Handle %p", handle);
		if (pager_output("\n"))
			break;
		/* device path */

		status = BS->ProtocolsPerHandle(handle, &protocols, &nproto);
		if (EFI_ERROR(status)) {
			snprintf(command_errbuf, sizeof (command_errbuf),
			    "ProtocolsPerHandle() error: %lld",
			    (long long)status);
			continue;
		}
		for (j = 0; j < nproto; j++) {
			for (k = 0; proto_names[k].name != NULL; k++)
				if (memcmp(protocols[j], &proto_names[k].guid,
				    sizeof (proto_names[k].guid)) == 0)
					break;
			if (proto_names[k].name != NULL)
				printf("  %s", proto_names[k].name);
			else
				printf("  %s", guid_to_string(protocols[j]));
			ret = pager_output("\n");
			if (ret)
				break;
		}
		BS->FreePool(protocols);
		if (ret)
			break;
	}
	pager_close();
	free(buffer);
	return (CMD_OK);
}

#ifdef EFI_ZFS_BOOT
COMMAND_SET(lszfs, "lszfs", "list child datasets of a zfs dataset",
    command_lszfs);

static int
command_lszfs(int argc, char *argv[])
{
	int err;

	if (argc != 2) {
		command_errmsg = "wrong number of arguments";
		return (CMD_ERROR);
	}

	err = zfs_list(argv[1]);
	if (err != 0) {
		command_errmsg = strerror(err);
		return (CMD_ERROR);
	}
	return (CMD_OK);
}

#ifdef __FreeBSD__
COMMAND_SET(reloadbe, "reloadbe", "refresh the list of ZFS Boot Environments",
	    command_reloadbe);

static int
command_reloadbe(int argc, char *argv[])
{
	int err;
	char *root;

	if (argc > 2) {
		command_errmsg = "wrong number of arguments";
		return (CMD_ERROR);
	}

	if (argc == 2) {
		err = zfs_bootenv(argv[1]);
	} else {
		root = getenv("zfs_be_root");
		if (root == NULL) {
			return (CMD_OK);
		}
		err = zfs_bootenv(root);
	}

	if (err != 0) {
		command_errmsg = strerror(err);
		return (CMD_ERROR);
	}

	return (CMD_OK);
}
#endif /* __FreeBSD__ */
#endif


COMMAND_SET(efishow, "efi-show", "print some or all EFI variables",
    command_efi_show);

static int
efi_print_var(CHAR16 *varnamearg, EFI_GUID *matchguid, int lflag)
{
	UINTN		datasz, i;
	EFI_STATUS	status;
	UINT32		attr;
	CHAR16		*data;
	char		*str;
	uint32_t	uuid_status;
	int		is_ascii;

	datasz = 0;
	status = RS->GetVariable(varnamearg, matchguid, &attr,
	    &datasz, NULL);
	if (status != EFI_BUFFER_TOO_SMALL) {
		printf("Can't get the variable: error %lu\n",
		    EFI_ERROR_CODE(status));
		return (CMD_ERROR);
	}
	data = malloc(datasz);
	status = RS->GetVariable(varnamearg, matchguid, &attr,
	    &datasz, data);
	if (status != EFI_SUCCESS) {
		printf("Can't get the variable: error %lu\n",
		    EFI_ERROR_CODE(status));
		return (CMD_ERROR);
	}
	uuid_status = uuid_s_ok;
	uuid_to_string((uuid_t *)matchguid, &str, &uuid_status);
	if (uuid_status != uuid_s_ok) {
		printf("Can't convert uuid to string: error %x\n", uuid_status);
		free(data);
		return (CMD_ERROR);
	}

	printf("%s 0x%x %S", str, attr, varnamearg);
	free(str);

	if (lflag == 0) {
		printf("=");
		is_ascii = 1;
		str = (char *)data;
		for (i = 0; i < datasz - 1; i++) {
			/*
			 * Quick hack to see if this ascii-ish string printable
			 * range plus tab, cr and lf
			 */
			if ((str[i] < 32 || str[i] > 126) && str[i] != 9 &&
			    str[i] != 10 && str[i] != 13) {
				is_ascii = 0;
				break;
			}
		}
		if (str[datasz - 1] != '\0')
			is_ascii = 0;
		if (is_ascii)
			printf("%s", str);
		else {
			printf("%lu bytes of data", (unsigned long)datasz);
#if 0
			/*
			 * Variables can have different type of data,
			 * byte, 2 bytes, etc, also device path and other.
			 * To display, the process actually depends on
			 * uuid and variable name. Note we can not blindly
			 * read the memory by 2 bytes, this seems to confuse
			 * at least vmware fusion.
			 */
			for (i = 0; i < datasz; i++) {
				char buf[5];

				if (isalnum(str[i]) || isspace(str[i]))
					snprintf(buf, sizeof (buf), "%c",
					    str[i]);
				else
					snprintf(buf, sizeof (buf), "\\x%02x",
					    str[i]);
				if ((rv = pager_output(buf)) != 0)
					break;
			}
#endif
		}
	}
	free(data);
	if (pager_output("\n"))
		return (CMD_WARN);
	return (CMD_OK);
}

static int
command_efi_show(int argc, char *argv[])
{
	/*
	 * efi-show [-a]
	 *      print all the env
	 * efi-show -u UUID
	 *      print all the env vars tagged with UUID
	 * efi-show -v var
	 *      search all the env vars and print the ones matching var
	 * eif-show -u UUID -v var
	 * eif-show UUID var
	 *      print all the env vars that match UUID and var
	 */
	/* NB: We assume EFI_GUID is the same as uuid_t */
	int		aflag = 0, gflag = 0, lflag = 0, vflag = 0;
	int		ch, rv;
	unsigned	i;
	EFI_STATUS	status;
	EFI_GUID	varguid = { 0,0,0,{0,0,0,0,0,0,0,0} };
	EFI_GUID	matchguid = { 0,0,0,{0,0,0,0,0,0,0,0} };
	uint32_t	uuid_status;
	CHAR16		varname[128];
	CHAR16		varnamearg[128];
	UINTN		varsz;

	optind = 1;
	optreset = 1;

	while ((ch = getopt(argc, argv, "ag:lv:")) != -1) {
		switch (ch) {
		case 'a':
			aflag = 1;
			break;
		case 'g':
			gflag = 1;
			uuid_from_string(optarg, (uuid_t *)&matchguid,
			    &uuid_status);
			if (uuid_status != uuid_s_ok) {
				printf("uid %s could not be parsed\n", optarg);
				return (CMD_ERROR);
			}
			break;
		case 'l':
			lflag = 1;
			break;
		case 'v':
			vflag = 1;
			if (strlen(optarg) >= nitems(varnamearg)) {
				printf("Variable %s is longer than %zd "
				    "characters\n", optarg, nitems(varnamearg));
				return (CMD_ERROR);
			}
			for (i = 0; i < strlen(optarg); i++)
				varnamearg[i] = optarg[i];
			varnamearg[i] = 0;
			break;
		default:
			printf("Invalid argument %c\n", ch);
			return (CMD_ERROR);
		}
	}

	if (aflag && (gflag || vflag)) {
		printf("-a isn't compatible with -v or -u\n");
		return (CMD_ERROR);
	}

	if (aflag && optind < argc) {
		printf("-a doesn't take any args");
		return (CMD_ERROR);
	}

	if (optind == argc)
		aflag = 1;

	argc -= optind;
	argv += optind;

	pager_open();
	if (vflag && gflag) {
		rv = efi_print_var(varnamearg, &matchguid, lflag);
		pager_close();
		return (rv);
	}

	if (argc == 2) {
		optarg = argv[0];
		if (strlen(optarg) >= nitems(varnamearg)) {
			printf("Variable %s is longer than %zd characters\n",
			    optarg, nitems(varnamearg));
			pager_close();
			return (CMD_ERROR);
		}
		for (i = 0; i < strlen(optarg); i++)
			varnamearg[i] = optarg[i];
		varnamearg[i] = 0;
		optarg = argv[1];
		uuid_from_string(optarg, (uuid_t *)&matchguid, &uuid_status);
		if (uuid_status != uuid_s_ok) {
			printf("uid %s could not be parsed\n", optarg);
			pager_close();
			return (CMD_ERROR);
		}
		rv = efi_print_var(varnamearg, &matchguid, lflag);
		pager_close();
		return (rv);
	}

	if (argc != 0) {
		printf("Too many args\n");
		pager_close();
		return (CMD_ERROR);
	}

	/*
	 * Initiate the search -- note the standard takes pain
	 * to specify the initial call must be a poiner to a NULL
	 * character.
	 */
	varsz = nitems(varname);
	varname[0] = 0;
	while ((status = RS->GetNextVariableName(&varsz, varname, &varguid)) !=
	    EFI_NOT_FOUND) {
		/* GetNextVariableName will set varsz, reset it */
		varsz = nitems(varname);
		if (aflag) {
			if (efi_print_var(varname, &varguid, lflag) != CMD_OK)
				break;
			continue;
		}
		if (vflag) {
			if (wcscmp(varnamearg, varname) == 0) {
				rv = efi_print_var(varname, &varguid, lflag);
				if (rv != CMD_OK)
					break;
				continue;
			}
		}
		if (gflag) {
			rv = memcmp(&varguid, &matchguid, sizeof(varguid));
			if (rv == 0) {
				rv = efi_print_var(varname, &varguid, lflag);
				if (rv != CMD_OK)
					break;
				continue;
			}
		}
	}
	pager_close();

	return (CMD_OK);
}

COMMAND_SET(efiset, "efi-set", "set EFI variables", command_efi_set);

static int
command_efi_set(int argc, char *argv[])
{
	char *uuid, *var, *val;
	CHAR16 wvar[128];
	EFI_GUID guid;
	uint32_t status;
	EFI_STATUS err;

	if (argc != 4) {
		printf("efi-set uuid var new-value\n");
		return (CMD_ERROR);
	}
	uuid = argv[1];
	var = argv[2];
	val = argv[3];
	uuid_from_string(uuid, (uuid_t *)&guid, &status);
	if (status != uuid_s_ok) {
		printf("Invalid uuid %s %d\n", uuid, status);
		return (CMD_ERROR);
	}
	cpy8to16(var, wvar, sizeof(wvar));
	err = RS->SetVariable(wvar, &guid, EFI_VARIABLE_NON_VOLATILE |
	    EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_BOOTSERVICE_ACCESS,
	    strlen(val) + 1, val);
	if (EFI_ERROR(err)) {
		printf("Failed to set variable: error %lu\n",
		    EFI_ERROR_CODE(err));
		return (CMD_ERROR);
	}
	return (CMD_OK);
}

COMMAND_SET(efiunset, "efi-unset", "delete / unset EFI variables",
    command_efi_unset);

static int
command_efi_unset(int argc, char *argv[])
{
	char *uuid, *var;
	CHAR16 wvar[128];
	EFI_GUID guid;
	uint32_t status;
	EFI_STATUS err;

	if (argc != 3) {
		printf("efi-unset uuid var\n");
		return (CMD_ERROR);
	}
	uuid = argv[1];
	var = argv[2];
	uuid_from_string(uuid, (uuid_t *)&guid, &status);
	if (status != uuid_s_ok) {
		printf("Invalid uuid %s\n", uuid);
		return (CMD_ERROR);
	}
	cpy8to16(var, wvar, sizeof(wvar));
	err = RS->SetVariable(wvar, &guid, 0, 0, NULL);
	if (EFI_ERROR(err)) {
		printf("Failed to unset variable: error %lu\n",
		    EFI_ERROR_CODE(err));
		return (CMD_ERROR);
	}
	return (CMD_OK);
}

#ifdef LOADER_FDT_SUPPORT
extern int command_fdt_internal(int argc, char *argv[]);

/*
 * Since proper fdt command handling function is defined in fdt_loader_cmd.c,
 * and declaring it as extern is in contradiction with COMMAND_SET() macro
 * (which uses static pointer), we're defining wrapper function, which
 * calls the proper fdt handling routine.
 */
static int
command_fdt(int argc, char *argv[])
{
	return (command_fdt_internal(argc, argv));
}

COMMAND_SET(fdt, "fdt", "flattened device tree handling", command_fdt);
#endif

/*
 * Chain load another efi loader
 */
static int
command_chain(int argc, char *argv[])
{
	EFI_GUID LoadedImageGUID = LOADED_IMAGE_PROTOCOL;
	EFI_HANDLE loaderhandle;
	EFI_LOADED_IMAGE *loaded_image;
	EFI_STATUS status;
	struct stat st;
	struct devdesc *dev;
	char *name, *path;
	void *buf;
	int fd;

	if (argc < 2) {
		command_errmsg = "wrong number of arguments";
		return (CMD_ERROR);
	}

	name = argv[1];

	if ((fd = open(name, O_RDONLY)) < 0) {
		command_errmsg = "no such file";
		return (CMD_ERROR);
	}

	if (fstat(fd, &st) < -1) {
		command_errmsg = "stat failed";
		close(fd);
		return (CMD_ERROR);
	}

	status = BS->AllocatePool(EfiLoaderCode, (UINTN)st.st_size, &buf);
	if (status != EFI_SUCCESS) {
		command_errmsg = "failed to allocate buffer";
		close(fd);
		return (CMD_ERROR);
	}
	if (read(fd, buf, st.st_size) != st.st_size) {
		command_errmsg = "error while reading the file";
		(void)BS->FreePool(buf);
		close(fd);
		return (CMD_ERROR);
	}
	close(fd);
	status = BS->LoadImage(FALSE, IH, NULL, buf, st.st_size, &loaderhandle);
	(void)BS->FreePool(buf);
	if (status != EFI_SUCCESS) {
		command_errmsg = "LoadImage failed";
		return (CMD_ERROR);
	}
	status = BS->HandleProtocol(loaderhandle, &LoadedImageGUID,
	    (void **)&loaded_image);

	if (argc > 2) {
		int i, len = 0;
		CHAR16 *argp;

		for (i = 2; i < argc; i++)
			len += strlen(argv[i]) + 1;

		len *= sizeof (*argp);
		loaded_image->LoadOptions = argp = malloc (len);
		loaded_image->LoadOptionsSize = len;
		for (i = 2; i < argc; i++) {
			char *ptr = argv[i];
			while (*ptr)
				*(argp++) = *(ptr++);
			*(argp++) = ' ';
		}
		*(--argv) = 0;
	}

	if (efi_getdev((void **)&dev, name, (const char **)&path) == 0)
		loaded_image->DeviceHandle =
		    efi_find_handle(dev->d_dev, dev->d_unit);

	dev_cleanup();
	status = BS->StartImage(loaderhandle, NULL, NULL);
	if (status != EFI_SUCCESS) {
		command_errmsg = "StartImage failed";
		free(loaded_image->LoadOptions);
		loaded_image->LoadOptions = NULL;
		status = BS->UnloadImage(loaded_image);
		return (CMD_ERROR);
	}

	return (CMD_ERROR);	/* not reached */
}

COMMAND_SET(chain, "chain", "chain load file", command_chain);

#ifdef EFI_ZFS_BOOT
static void
efi_zfs_probe(void)
{
	EFI_HANDLE h;
	u_int unit;
	int i;
	char dname[SPECNAMELEN + 1];
	uint64_t guid;

	unit = 0;
	h = efi_find_handle(&efipart_dev, 0);
	for (i = 0; h != NULL; h = efi_find_handle(&efipart_dev, ++i)) {
		snprintf(dname, sizeof(dname), "%s%d:", efipart_dev.dv_name, i);
		if (zfs_probe_dev(dname, &guid) == 0)
			(void)efi_handle_update_dev(h, &zfs_dev, unit++, guid);
	}
}
#endif
