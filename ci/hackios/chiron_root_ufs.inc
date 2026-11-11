/*
 * ---- chiron: the iOS root volume kept in a UFS partition --------------------------------
 *
 * "oem ramdisk" makes the host send a root image over USB into the resident area. A 2.4 GB
 * volume takes 73 s, and the loader forgets it on every reset, so a kernel iteration would
 * pay that again and again. These commands move the volume into a flash partition instead:
 *
 *   oem writesys   write what is resident into the "system" partition (host sends it once)
 *   oem rootufs    fill the resident area from that partition instead of the USB wire
 *
 * The size is fixed here on purpose: it is one image, whose identity the project records
 * (system.apfs, 2,492,219,392 bytes, sha256 f781f611...f65fa4). Only that prefix is touched.
 */
#define CHIRON_ROOT_NAME    u"system"
#define CHIRON_ROOT_BYTES   2492219392ULL
#define CHIRON_ROOT_CHUNK   (8ULL << 20)

/* GPT name match, for the partition whose label is exactly CHIRON_ROOT_NAME. */
static int
chiron_root_named(const efi_partition_info *info)
{
	static const char16 want[] = CHIRON_ROOT_NAME;

	for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
		if (info->name[k] != want[k]) {
			return 0;
		}
	}
	return 1;
}

/* The partition the root volume lives in, found the same way the log partition is. */
static efi_block_io *
chiron_root_partition(efi_partition_info **out_info)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		if (!chiron_root_named(info)) {
			continue;
		}
		if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		if (out_info != NULL) {
			*out_info = info;
		}
		return bio;
	}
	return NULL;
}

/* The whole-disk block device the partition sits on (largest media that covers it). */
static efi_block_io *
chiron_disk_above(uint64_t blocks)
{
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;
	efi_block_io *best = NULL;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_block_io *bio;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
			continue;
		}
		if (bio->media->last_block + 1 < blocks) {
			continue;
		}
		if (best == NULL || bio->media->last_block > best->media->last_block) {
			best = bio;
		}
	}
	return best;
}

/* Fill the resident area from the partition. 1 = the resident root is in place. */
static int
chiron_root_from_ufs(void)
{
	struct payload_info pi;
	efi_block_io *bio;
	uint64_t bs, blocks, done, started, limit;
	uint8_t *dest;

	bio = chiron_root_partition(NULL);
	if (bio == NULL) {
		print("root: no partition named system\n");
		return 0;
	}
	bs = bio->media->block_size;
	blocks = bio->media->last_block + 1;
	if (win_size == 0) {
		parse_payload(&pi, 0);
		if (!choose_window(&pi)) {
			print("root: no memory window\n");
			return 0;
		}
	}
	limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
	if (CHIRON_ROOT_BYTES > limit || bs == 0 || CHIRON_ROOT_BYTES > blocks * bs) {
		print("root: %d bytes do not fit (window leaves %d, the partition has %d blocks of %d)\n",
		    CHIRON_ROOT_BYTES, limit, blocks, bs);
		return 0;
	}
	dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
	resident_rd_size = 0;
	print("root: reading %d MB from 'system', %d bytes per block\n", CHIRON_ROOT_BYTES >> 20, bs);
	started = READ_SYSREG(CNTVCT_EL0);
	for (done = 0; done < CHIRON_ROOT_BYTES; done += CHIRON_ROOT_CHUNK) {
		uint64_t want = CHIRON_ROOT_BYTES - done;

		if (want > CHIRON_ROOT_CHUNK) {
			want = CHIRON_ROOT_CHUNK;
		}
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, done / bs, want / bs, dest + done))) {
			print("root: read failed at block %d\n", done / bs);
			resident_rd_size = 0;
			return 0;
		}
		if ((done >> 20) % 256 == 0) {
			print("root: %d of %d MB\n", done >> 20, CHIRON_ROOT_BYTES >> 20);
		}
	}
	clean_dcache((uint64_t)(uintptr_t)dest, (uint64_t)(uintptr_t)dest + CHIRON_ROOT_BYTES);
	resident_rd_size = CHIRON_ROOT_BYTES;
	print("root: %d MB in place in %d ms, the volume begins %c%c%c%c\n", CHIRON_ROOT_BYTES >> 20,
	    (READ_SYSREG(CNTVCT_EL0) - started) / 19200, dest[32], dest[33], dest[34], dest[35]);
	return 1;
}

/*
 * Put the volume that is already in the resident area into the partition. A plain
 * write_blocks is tried first; the resident bytes were produced by the CPU, so every chunk is
 * cleaned to memory before the controller's DMA reads it. If the partition driver refuses
 * writes, the same bytes go to the whole disk at the partition's own start LBA.
 */
static int
chiron_root_to_ufs(void)
{
	efi_partition_info *info = NULL;
	efi_block_io *bio, *disk = NULL;
	uint64_t done, started, chunk, start_lba = 0, status, disk_mode = 0;

	bio = chiron_root_partition(&info);
	if (bio == NULL) {
		print("root: no partition named system\n");
		return 0;
	}
	if (resident_rd_size == 0) {
		print("root: nothing is resident; send the volume with 'oem ramdisk' first\n");
		return 0;
	}
	print("root: 'system' has %d blocks of %d, read_only %d, logical %d\n", bio->media->last_block + 1,
	    bio->media->block_size, (uint64_t)bio->media->read_only, (uint64_t)bio->media->logical);
	if (resident_rd_size > (bio->media->last_block + 1) * bio->media->block_size) {
		print("root: %d bytes do not fit in the partition\n", resident_rd_size);
		return 0;
	}
	print("root: writing %d MB to 'system'\n", resident_rd_size >> 20);
	started = READ_SYSREG(CNTVCT_EL0);
	for (done = 0; done < resident_rd_size; done += chunk) {
		uint8_t *src = (uint8_t *)(win_base + RESIDENT_RD_OFFSET) + done;

		chunk = CHIRON_ROOT_CHUNK;
		if (resident_rd_size - done < chunk) {
			chunk = resident_rd_size - done;
		}
		clean_dcache((uint64_t)(uintptr_t)src, (uint64_t)(uintptr_t)src + chunk);
		if (disk_mode == 0) {
			status = bio->write_blocks(bio, bio->media->media_id, done / bio->media->block_size,
			    chunk / bio->media->block_size, src);
			if (!EFI_ERROR(status)) {
				goto written;
			}
			print("root: the partition refused the write (%x); going through the disk\n", status);
			if (info == NULL) {
				print("root: no partition info to locate it on the disk\n");
				return 0;
			}
			disk = chiron_disk_above(info->end_lba + 1);
			if (disk == NULL) {
				print("root: no whole-disk block device found\n");
				return 0;
			}
			start_lba = info->start_lba;
			disk_mode = 1;
			print("root: disk %d blocks of %d, the partition starts at block %d\n",
			    disk->media->last_block + 1, disk->media->block_size, start_lba);
		}
		status = disk->write_blocks(disk, disk->media->media_id, start_lba + done / disk->media->block_size,
		    chunk / disk->media->block_size, src);
		if (EFI_ERROR(status)) {
			print("root: write failed at block %d (%x)\n", done / disk->media->block_size, status);
			return 0;
		}
written:
		if ((done >> 20) % 256 == 0) {
			print("root: %d of %d MB written\n", done >> 20, resident_rd_size >> 20);
		}
	}
	print("root: %d MB written in %d ms across %d chunks\n", resident_rd_size >> 20,
	    (READ_SYSREG(CNTVCT_EL0) - started) / 19200, resident_rd_size / CHIRON_ROOT_CHUNK + 1);
	return 1;
}
