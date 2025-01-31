/*
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-logitech-hidpp-bootloader-nordic.h"
#include "fu-logitech-hidpp-common.h"
#include "fu-logitech-hidpp-struct.h"

struct _FuLogitechHidppBootloaderNordic {
	FuLogitechHidppBootloader parent_instance;
};

G_DEFINE_TYPE(FuLogitechHidppBootloaderNordic,
	      fu_logitech_hidpp_bootloader_nordic,
	      FU_TYPE_LOGITECH_HIDPP_BOOTLOADER)

static gchar *
fu_logitech_hidpp_bootloader_nordic_get_hw_platform_id(FuLogitechHidppBootloader *self,
						       GError **error)
{
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_GET_HW_PLATFORM_ID;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to get HW ID: ");
		return NULL;
	}
	return g_strndup((const gchar *)req->data, req->len);
}

static gchar *
fu_logitech_hidpp_bootloader_nordic_get_fw_version(FuLogitechHidppBootloader *self, GError **error)
{
	guint16 micro;

	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_GET_FW_VERSION;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to get firmware version: ");
		return NULL;
	}

	/* RRRxx.yy_Bzzzz
	 * 012345678901234*/
	micro = (guint16)fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 10) << 8;
	micro += fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 12);
	return fu_logitech_hidpp_format_version(
	    "RQR",
	    fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 3),
	    fu_logitech_hidpp_buffer_read_uint8((const gchar *)req->data + 6),
	    micro);
}

static gboolean
fu_logitech_hidpp_bootloader_nordic_setup(FuDevice *device, GError **error)
{
	FuLogitechHidppBootloader *self = FU_LOGITECH_HIDPP_BOOTLOADER(device);
	g_autofree gchar *hw_platform_id = NULL;
	g_autofree gchar *version_fw = NULL;
	g_autoptr(GError) error_local = NULL;

	/* FuLogitechHidppBootloader->setup */
	if (!FU_DEVICE_CLASS(fu_logitech_hidpp_bootloader_nordic_parent_class)
		 ->setup(device, error))
		return FALSE;

	/* get MCU */
	hw_platform_id = fu_logitech_hidpp_bootloader_nordic_get_hw_platform_id(self, error);
	if (hw_platform_id == NULL)
		return FALSE;
	g_debug("hw-platform-id=%s", hw_platform_id);

	/* get firmware version, which is not fatal */
	version_fw = fu_logitech_hidpp_bootloader_nordic_get_fw_version(self, &error_local);
	if (version_fw == NULL) {
		g_warning("failed to get firmware version: %s", error_local->message);
		fu_device_set_version(device, "RQR12.00_B0000");
	} else {
		fu_device_set_version(device, version_fw);
	}

	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_nordic_write_signature(FuLogitechHidppBootloader *self,
						    guint16 addr,
						    guint8 len,
						    const guint8 *data,
						    GError **error)
{
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = 0xC0;
	req->addr = addr;
	req->len = len;
	memcpy(req->data, data, req->len);
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to write sig @0x%02x: ", addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_RAM_BUFFER_INVALID_ADDR) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: signature is too big",
			    addr);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_nordic_write(FuLogitechHidppBootloader *self,
					  guint16 addr,
					  guint8 len,
					  const guint8 *data,
					  GError **error)
{
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE;
	req->addr = addr;
	req->len = len;
	if (req->len > 28) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: data length too large %02x",
			    addr,
			    req->len);
		return FALSE;
	}
	memcpy(req->data, data, req->len);
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to transfer fw @0x%02x: ", addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_INVALID_ADDR) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: invalid address",
			    addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_VERIFY_FAIL) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: failed to verify flash content",
			    addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_NONZERO_START) {
		g_debug("wrote %d bytes at address %04x, value %02x",
			req->len,
			req->addr,
			req->data[0]);
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: only 1 byte write of 0xff supported",
			    addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_INVALID_CRC) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to write @%04x: invalid CRC",
			    addr);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_nordic_erase(FuLogitechHidppBootloader *self,
					  guint16 addr,
					  GError **error)
{
	g_autoptr(FuLogitechHidppBootloaderRequest) req =
	    fu_logitech_hidpp_bootloader_request_new();
	req->cmd = FU_LOGITECH_HIDPP_BOOTLOADER_CMD_ERASE_PAGE;
	req->addr = addr;
	req->len = 0x01;
	if (!fu_logitech_hidpp_bootloader_request(self, req, error)) {
		g_prefix_error(error, "failed to erase fw @0x%02x: ", addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_ERASE_PAGE_INVALID_ADDR) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to erase @%04x: invalid page",
			    addr);
		return FALSE;
	}
	if (req->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_ERASE_PAGE_NONZERO_START) {
		g_set_error(error,
			    G_IO_ERROR,
			    G_IO_ERROR_FAILED,
			    "failed to erase @%04x: byte 0x00 is not 0xff",
			    addr);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_logitech_hidpp_bootloader_nordic_write_firmware(FuDevice *device,
						   FuFirmware *firmware,
						   FuProgress *progress,
						   FwupdInstallFlags flags,
						   GError **error)
{
	FuLogitechHidppBootloader *self = FU_LOGITECH_HIDPP_BOOTLOADER(device);
	const FuLogitechHidppBootloaderRequest *payload;
	guint16 addr;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GPtrArray) reqs = NULL;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	if (fu_device_has_private_flag(device, FU_LOGITECH_HIDPP_BOOTLOADER_FLAG_IS_SIGNED)) {
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_ERASE, 4, NULL);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 13, NULL);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "device-write0");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 82, "reset vector");
	} else {
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_ERASE, 22, NULL);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 72, NULL);
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 1, "device-write0");
		fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 6, "reset-vector");
	}

	/* get default image */
	fw = fu_firmware_get_bytes(firmware, error);
	if (fw == NULL)
		return FALSE;

	/* erase firmware pages up to the bootloader */
	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_ERASE);
	for (addr = fu_logitech_hidpp_bootloader_get_addr_lo(self);
	     addr < fu_logitech_hidpp_bootloader_get_addr_hi(self);
	     addr += fu_logitech_hidpp_bootloader_get_blocksize(self)) {
		if (!fu_logitech_hidpp_bootloader_nordic_erase(self, addr, error))
			return FALSE;
	}
	fu_progress_step_done(progress);

	/* transfer payload */
	reqs = fu_logitech_hidpp_bootloader_parse_requests(self, fw, error);
	if (reqs == NULL)
		return FALSE;
	fu_progress_set_status(progress, FWUPD_STATUS_DEVICE_WRITE);
	for (guint i = 1; i < reqs->len; i++) {
		gboolean res;
		payload = g_ptr_array_index(reqs, i);

		if (payload->cmd == FU_LOGITECH_HIDPP_BOOTLOADER_CMD_WRITE_SIGNATURE) {
			res = fu_logitech_hidpp_bootloader_nordic_write_signature(self,
										  payload->addr,
										  payload->len,
										  payload->data,
										  error);
		} else {
			res = fu_logitech_hidpp_bootloader_nordic_write(self,
									payload->addr,
									payload->len,
									payload->data,
									error);
		}

		if (!res)
			return FALSE;
		fu_progress_set_percentage_full(fu_progress_get_child(progress), i + 1, reqs->len);
	}
	fu_progress_step_done(progress);

	/* send the first managed packet last, excluding the reset vector */
	payload = g_ptr_array_index(reqs, 0);
	if (!fu_logitech_hidpp_bootloader_nordic_write(self,
						       payload->addr + 1,
						       payload->len - 1,
						       payload->data + 1,
						       error))
		return FALSE;
	fu_progress_step_done(progress);

	/* reset vector */
	if (!fu_logitech_hidpp_bootloader_nordic_write(self, 0x0000, 0x01, payload->data, error))
		return FALSE;
	fu_progress_step_done(progress);

	/* success! */
	return TRUE;
}

static void
fu_logitech_hidpp_bootloader_nordic_class_init(FuLogitechHidppBootloaderNordicClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS(klass);
	klass_device->write_firmware = fu_logitech_hidpp_bootloader_nordic_write_firmware;
	klass_device->setup = fu_logitech_hidpp_bootloader_nordic_setup;
}

static void
fu_logitech_hidpp_bootloader_nordic_init(FuLogitechHidppBootloaderNordic *self)
{
}
