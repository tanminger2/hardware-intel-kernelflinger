/*
 * Copyright (c) 2015, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <efi.h>
#include <efilib.h>
#include <log.h>
#include <lib.h>
#include "storage.h"
#include "pci.h"

static struct storage *cur_storage;
static PCI_DEVICE_PATH boot_device = { .Function = -1, .Device = -1 };
static enum storage_type boot_device_type;
static BOOLEAN initialized = FALSE;

static BOOLEAN is_boot_device(EFI_DEVICE_PATH *p)
{
	PCI_DEVICE_PATH *pci;

	if (boot_device.Header.Type == 0)
		return FALSE;

	pci = get_pci_device_path(p);

	return pci && pci->Function == boot_device.Function
		&& pci->Device == boot_device.Device;
}

extern struct storage STORAGE(STORAGE_EMMC);
extern struct storage STORAGE(STORAGE_UFS);
extern struct storage STORAGE(STORAGE_SDCARD);
extern struct storage STORAGE(STORAGE_SATA);

static EFI_STATUS identify_storage(EFI_DEVICE_PATH *device_path,
				   enum storage_type filter,
				   struct storage **storage,
				   enum storage_type *type)
{
	enum storage_type st;
	static struct storage *supported_storage[STORAGE_ALL] =  {
		&STORAGE(STORAGE_EMMC),
		&STORAGE(STORAGE_UFS),
		&STORAGE(STORAGE_SDCARD),
		&STORAGE(STORAGE_SATA)
	};

	for (st = STORAGE_EMMC; st < STORAGE_ALL; st++) {
		if ((filter == st || filter == STORAGE_ALL) &&
		    supported_storage[st] && supported_storage[st]->probe(device_path)) {
			debug(L"%s storage identified", supported_storage[st]->name);
			*storage = supported_storage[st];
			*type = st;
			return EFI_SUCCESS;
		}
	}

	return EFI_UNSUPPORTED;
}

EFI_STATUS identify_boot_device(enum storage_type filter)
{
	EFI_STATUS ret;
	EFI_HANDLE *handles;
	UINTN nb_handle = 0;
	UINTN i;
	EFI_DEVICE_PATH *device_path;
	PCI_DEVICE_PATH *pci = NULL;
	struct storage *storage;
	enum storage_type type;

	cur_storage = NULL;
	ret = uefi_call_wrapper(BS->LocateHandleBuffer, 5, ByProtocol,
				&BlockIoProtocol, NULL, &nb_handle, &handles);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to locate Block IO Protocol");
		return ret;
	}

	boot_device.Header.Type = 0;
	for (i = 0; i < nb_handle; i++) {
		device_path = DevicePathFromHandle(handles[i]);
		if (!device_path)
			continue;

		pci = get_pci_device_path(device_path);
		if (!pci)
			continue;

		if (boot_device.Function == pci->Function &&
		    boot_device.Device == pci->Device)
			continue;

		ret = identify_storage(device_path, filter, &storage, &type);
		if (EFI_ERROR(ret))
			continue;

		if (!boot_device.Header.Type || boot_device_type > type) {
			memcpy(&boot_device, pci, sizeof(boot_device));
			boot_device_type = type;
			cur_storage = storage;
			continue;
		}

		if (boot_device_type == type) {
			error(L"Multiple identifcal storage found! Can't make a decision");
			cur_storage = NULL;
			boot_device.Header.Type = 0;
			FreePool(handles);
			return EFI_UNSUPPORTED;
		}
	}

	FreePool(handles);

	if (!cur_storage) {
		error(L"No PCI storage found");
		return EFI_UNSUPPORTED;
	}

	debug(L"%s storage selected", cur_storage->name);
	return EFI_SUCCESS;
}

static BOOLEAN valid_storage(void)
{
	if (!initialized) {
		initialized = TRUE;
		return !EFI_ERROR(identify_boot_device(STORAGE_ALL));
	}
	return boot_device.Header.Type && cur_storage;
}

EFI_STATUS storage_check_logical_unit(EFI_DEVICE_PATH *p, logical_unit_t log_unit)
{
	if (!valid_storage())
		return EFI_UNSUPPORTED;
	if (!is_boot_device(p))
		return EFI_UNSUPPORTED;

	return cur_storage->check_logical_unit(p, log_unit);
}

EFI_STATUS storage_erase_blocks(EFI_HANDLE handle, EFI_BLOCK_IO *bio, EFI_LBA start, EFI_LBA end)
{
	if (!valid_storage())
		return EFI_UNSUPPORTED;

	debug(L"Erase lba %ld -> %ld", start, end);
	return cur_storage->erase_blocks(handle, bio, start, end);
}

#define percent5(x, max) (x) * 20 / (max) * 5

EFI_STATUS fill_with(EFI_BLOCK_IO *bio, EFI_LBA start, EFI_LBA end,
			    VOID *pattern, UINTN pattern_blocks)
{
	EFI_LBA lba;
	UINT64 size;
	UINT64 prev = 0, progress = 0;
	EFI_STATUS ret;

	debug(L"Fill lba %d -> %d", start, end);
	if (end <= start)
		return EFI_INVALID_PARAMETER;

	for (lba = start; lba <= end; lba += pattern_blocks, prev = progress,
				      progress = percent5(lba - start, end - start)) {
		if (lba + pattern_blocks > end + 1)
			size = end - lba + 1;
		else
			size = pattern_blocks;

		if (progress != prev)
			debug(L"%d%% completed", progress);

		ret = uefi_call_wrapper(bio->WriteBlocks, 5, bio, bio->Media->MediaId, lba,
					bio->Media->BlockSize * size, pattern);
		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Failed to erase block %ld", lba);
			return ret;
		}
	}

	return EFI_SUCCESS;
}

EFI_STATUS fill_zero(EFI_BLOCK_IO *bio, EFI_LBA start, EFI_LBA end)
{
	EFI_STATUS ret;
	VOID *emptyblock;
	VOID *aligned_emptyblock;

	ret = alloc_aligned(&emptyblock, &aligned_emptyblock,
			    bio->Media->BlockSize * N_BLOCK,
			    bio->Media->IoAlign);
	if (EFI_ERROR(ret))
		return ret;

	ret = fill_with(bio, start, end, aligned_emptyblock, N_BLOCK);

	FreePool(emptyblock);

	return ret;
}

EFI_STATUS storage_set_boot_device(EFI_HANDLE device)
{
	EFI_DEVICE_PATH *device_path  = DevicePathFromHandle(device);
	PCI_DEVICE_PATH *pci;
	EFI_STATUS ret;
	CHAR16 *dps;

	if (!device_path) {
		error(L"Failed to get device path from boot handle");
		return EFI_UNSUPPORTED;
	}

	pci = get_pci_device_path(device_path);
	if (!pci) {
		error(L"Boot device is not PCI, unsupported");
		return EFI_UNSUPPORTED;
	}

	ret = identify_storage(device_path, STORAGE_ALL, &cur_storage,
			       &boot_device_type);
	if (EFI_ERROR(ret)) {
		error(L"Boot device unsupported");
		return ret;
	}
	dps = DevicePathToStr((EFI_DEVICE_PATH *)pci);
	debug(L"Setting PCI boot device to: %s", dps);
	FreePool(dps);

	initialized = TRUE;
	memcpy(&boot_device, pci, sizeof(boot_device));
	return EFI_SUCCESS;
}

PCI_DEVICE_PATH *get_boot_device(void)
{
	EFI_STATUS ret;

	if (!initialized) {
		ret = identify_boot_device(STORAGE_ALL);
		if (EFI_ERROR(ret))
			efi_perror(ret, L"Failed to get boot device");
	}
	return boot_device.Header.Type == 0 ? NULL : &boot_device;
}
