/*
 * LUKS - Linux Unified Key Setup v2, reencryption keyslot handler
 *
 * Copyright (C) 2016-2018, Red Hat, Inc. All rights reserved.
 * Copyright (C) 2016-2018, Ondrej Kozina
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "luks2_internal.h"

static int reenc_keyslot_open(struct crypt_device *cd,
	int keyslot,
	const char *password,
	size_t password_len,
	char *volume_key,
	size_t volume_key_len)
{
	return -ENOENT; /* TODO: */
}

int reenc_keyslot_alloc(struct crypt_device *cd,
	struct luks2_hdr *hdr,
	int keyslot,
	const struct crypt_params_reencrypt *params)
{
	int r;
	json_object *jobj_keyslots, *jobj_keyslot, *jobj_area;
	uint64_t area_offset, area_length;

	log_dbg(cd, "Allocating reencrypt keyslot %d.", keyslot);

	if (keyslot < 0 || keyslot >= LUKS2_KEYSLOTS_MAX)
		return -ENOMEM;

	if (!json_object_object_get_ex(hdr->jobj, "keyslots", &jobj_keyslots))
		return -EINVAL;

	/* encryption doesn't require area (we shift data and backup will be available) */
	if (!params->data_shift) {
		r = LUKS2_find_area_max_gap(cd, hdr, &area_offset, &area_length);
		if (r < 0)
			return r;
	} else { /* we can't have keyslot w/o area...bug? */
		r = LUKS2_find_area_gap(cd, hdr, 1, &area_offset, &area_length);
		if (r < 0)
			return r;
	}

	jobj_keyslot = json_object_new_object();
	if (!jobj_keyslot)
		return -ENOMEM;

	jobj_area = json_object_new_object();

	if (params->data_shift) {
		json_object_object_add(jobj_area, "type", json_object_new_string("datashift"));
		json_object_object_add(jobj_area, "shift_size", json_object_new_uint64(params->data_shift << SECTOR_SHIFT));
	} else
		/* except data shift protection, initial setting is irrelevant. Type can be changed during reencryption */
		json_object_object_add(jobj_area, "type", json_object_new_string("none"));

	json_object_object_add(jobj_area, "offset", json_object_new_uint64(area_offset));
	json_object_object_add(jobj_area, "size", json_object_new_uint64(area_length));

	json_object_object_add(jobj_keyslot, "type", json_object_new_string("reencrypt"));
	json_object_object_add(jobj_keyslot, "key_size", json_object_new_int(1)); /* useless but mandatory */
	json_object_object_add(jobj_keyslot, "mode", json_object_new_string(params->mode));
	if (params->direction == CRYPT_REENCRYPT_FORWARD)
		json_object_object_add(jobj_keyslot, "direction", json_object_new_string("forward"));
	else if (params->direction == CRYPT_REENCRYPT_BACKWARD)
		json_object_object_add(jobj_keyslot, "direction", json_object_new_string("backward"));
	else
		return -EINVAL;

	json_object_object_add(jobj_keyslot, "area", jobj_area);

	json_object_object_add_by_uint(jobj_keyslots, keyslot, jobj_keyslot);
	if (LUKS2_check_json_size(cd, hdr)) {
		log_dbg(cd, "New keyslot too large to fit in free metadata space.");
		json_object_object_del_by_uint(jobj_keyslots, keyslot);
		return -ENOSPC;
	}

	JSON_DBG(cd, hdr->jobj, "JSON:");

	return 0;
}

static int reenc_keyslot_store_data(struct crypt_device *cd,
	json_object *jobj_keyslot,
	const void *buffer, size_t buffer_len)
{
	int devfd, r;
	json_object *jobj_area, *jobj_offset, *jobj_length;
	uint64_t area_offset, area_length;
	struct device *device = crypt_metadata_device(cd);

	if (!json_object_object_get_ex(jobj_keyslot, "area", &jobj_area) ||
	    !json_object_object_get_ex(jobj_area, "offset", &jobj_offset) ||
	    !json_object_object_get_ex(jobj_area, "size", &jobj_length))
		return -EINVAL;

	area_offset = json_object_get_uint64(jobj_offset);
	area_length = json_object_get_uint64(jobj_length);

	if (!area_offset || !area_length || ((uint64_t)buffer_len > area_length))
		return -EINVAL;

	r = device_write_lock(cd, device);
	if (r) {
		log_err(cd, _("Failed to acquire write lock on device %s."),
			device_path(device));
		return r;
	}

	devfd = device_open_locked(cd, device, O_RDWR);
	if (devfd >= 0) {
		if (write_lseek_blockwise(devfd, device_block_size(cd, device),
					  device_alignment(device), CONST_CAST(void *)buffer,
					  buffer_len, area_offset) < 0)
			r = -EIO;
		else
			r = 0;
		close(devfd);
	} else
		r = -EINVAL;

	device_write_unlock(cd, device);

	if (r)
		log_err(cd, _("IO error while encrypting keyslot."));

	return r;
}

static int reenc_keyslot_store(struct crypt_device *cd,
	int keyslot,
	const char *password __attribute__((unused)),
	size_t password_len __attribute__((unused)),
	const char *buffer,
	size_t buffer_len)
{
	struct luks2_hdr *hdr;
	json_object *jobj_keyslot;
	int r = 0;

	if (!cd || !buffer || !buffer_len)
		return -EINVAL;

	if (!(hdr = crypt_get_hdr(cd, CRYPT_LUKS2)))
		return -EINVAL;

	log_dbg(cd, "Reencrypt keyslot %d store.", keyslot);

	jobj_keyslot = LUKS2_get_keyslot_jobj(hdr, keyslot);
	if (!jobj_keyslot)
		return -EINVAL;

	r = reenc_keyslot_store_data(cd, jobj_keyslot, buffer, buffer_len);
	if (r < 0)
		return r;

	r = LUKS2_hdr_write(cd, hdr);
	if (r < 0)
		return r;

	return keyslot;
}

int reenc_keyslot_update(struct crypt_device *cd,
	const struct luks2_reenc_context *rh)
{
	json_object *jobj_keyslot, *jobj_area, *jobj_area_type;
	struct luks2_hdr *hdr;

	if (!(hdr = crypt_get_hdr(cd, CRYPT_LUKS2)))
		return -EINVAL;

	jobj_keyslot = LUKS2_get_keyslot_jobj(hdr, rh->reenc_keyslot);
	if (!jobj_keyslot)
		return -EINVAL;

	json_object_object_get_ex(jobj_keyslot, "area", &jobj_area);
	json_object_object_get_ex(jobj_area, "type", &jobj_area_type);

	if (rh->rp.type == REENC_PROTECTION_CHECKSUM) {
		log_dbg(cd, "Updating reencrypt keyslot for checksum protection.");
		json_object_object_add(jobj_area, "type", json_object_new_string("checksum"));
		json_object_object_add(jobj_area, "hash", json_object_new_string(rh->rp.p.csum.hash));
		json_object_object_add(jobj_area, "sector_size", json_object_new_int64(rh->alignment));
	} else if (rh->rp.type == REENC_PROTECTION_NOOP) {
		log_dbg(cd, "Updating reencrypt keyslot for none protection.");
		json_object_object_add(jobj_area, "type", json_object_new_string("none"));
		json_object_object_del(jobj_area, "hash");
	} else if (rh->rp.type == REENC_PROTECTION_JOURNAL) {
		log_dbg(cd, "Updating reencrypt keyslot for journal protection.");
		json_object_object_add(jobj_area, "type", json_object_new_string("journal"));
		json_object_object_del(jobj_area, "hash");
	} else
		log_dbg(cd, "No update of reencrypt keyslot needed.");

	return 0;
}

static int reenc_keyslot_wipe(struct crypt_device *cd, int keyslot)
{
	return 0;
}

static int reenc_keyslot_dump(struct crypt_device *cd, int keyslot)
{
	return 0;
}

static int reenc_keyslot_validate(struct crypt_device *cd, json_object *jobj_keyslot)
{
	json_object *jobj_mode, *jobj_area, *jobj_type, *jobj_shift_size, *jobj_hash, *jobj_sector_size;
	const char *mode, *type;
	uint32_t sector_size;
	uint64_t shift_size;

	/* mode (string: encrypt,reencrypt,decrypt)
	 * direction (string:)
	 * area {
	 *   type: (string: datashift, journal, checksum, none)
	 *   	hash: (string: checksum only)
	 *   	sector_size (uint32: checksum only)
	 *   	shift_size (uint64: datashift only)
	 * }
	 */

	/* area and area type are validated in general validation code */
	if (!jobj_keyslot || !json_object_object_get_ex(jobj_keyslot, "area", &jobj_area) ||
	    !json_object_object_get_ex(jobj_area, "type", &jobj_type))
		return -EINVAL;

	jobj_mode = json_contains(cd, jobj_keyslot, "", "reencrypt keyslot", "mode", json_type_string);

	if (!jobj_mode || !json_contains(cd, jobj_keyslot, "", "reencrypt keyslot", "direction", json_type_string))
		return -EINVAL;

	mode = json_object_get_string(jobj_mode);
	type = json_object_get_string(jobj_type);

	if (strcmp(mode, "reencrypt") && strcmp(mode, "encrypt") &&
	    strcmp(mode, "decrypt")) {
		log_dbg(cd, "Illegal reencrypt mode %s.", mode);
		return -EINVAL;
	}

	if (!strcmp(type, "checksum")) {
		jobj_hash = json_contains(cd, jobj_area, "type:checksum", "Keyslot area", "hash", json_type_string);
		jobj_sector_size = json_contains(cd, jobj_area, "type:checksum", "Keyslot area", "sector_size", json_type_int);
		if (!jobj_hash || !jobj_sector_size)
			return -EINVAL;
		if (!validate_json_uint32(jobj_sector_size))
			return -EINVAL;
		sector_size = json_object_get_uint32(jobj_sector_size);
		if (sector_size < SECTOR_SIZE || NOTPOW2(sector_size)) {
			log_dbg(cd, "Invalid sector_size (%" PRIu32 ") for checksum resilience mode.", sector_size);
			return -EINVAL;
		}
	} else if (!strcmp(type, "datashift")) {
		if (!(jobj_shift_size = json_contains(cd, jobj_area, "type:datashift", "Keyslot area", "shift_size", json_type_string)))
			return -EINVAL;

		shift_size = json_object_get_uint64(jobj_shift_size);
		if (!shift_size)
			return -EINVAL;

		if (MISALIGNED_512(shift_size)) {
			log_dbg(cd, "Shift size field has to be aligned to sector size: %" PRIu32, SECTOR_SIZE);
			return -EINVAL;
		}
	}

	return 0;
}

const keyslot_handler reenc_keyslot = {
	.name  = "reencrypt",
	.open  = reenc_keyslot_open,
	.store = reenc_keyslot_store, /* initialization only or also per every chunk write */
	.wipe  = reenc_keyslot_wipe,
	.dump  = reenc_keyslot_dump,
	.validate  = reenc_keyslot_validate
};
