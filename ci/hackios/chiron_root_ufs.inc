/*
 * ---- chiron: the iOS root volume kept in the "darwin" UFS partition ----------------------
 *
 * "oem ramdisk" makes the host send a root image over USB into the resident area. A 2.4 GB
 * volume takes about 73 s, and the loader forgets it on every reset. This reads the same image
 * from the phone's own flash instead: the partition "darwin", which userdata-carve.py cut off
 * the end of Android's userdata and which darwin-write.py / EDL filled with system.apfs.
 *
 * The image begins at the partition's own block 0 (the volume was written there), and only its
 * recorded prefix is touched: system.apfs, 2,492,219,392 bytes, sha256 f781f611...f65fa4.
 *
 * Nothing is written to any partition here. There is no counterpart to "oem writesys": the
 * volume is put on the phone from the computer (EDL / darwin-write.py), never by this loader.
 */
#define CHIRON_ROOT_NAME    u"darwin"
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

/* Fill the resident area from the partition. 1 = the resident root is in place. */
static int
chiron_root_from_ufs(void)
{
	struct payload_info pi;
	efi_partition_info *info = NULL;
	efi_block_io *bio;
	uint64_t bs, first_lba, blocks, done, started, limit;
	uint8_t *dest;

	bio = chiron_root_partition(&info);
	if (bio == NULL) {
		print("root: no partition named darwin; the host must send the volume\n");
		return 0;
	}
	bs = bio->media->block_size;
	blocks = bio->media->last_block + 1;
	first_lba = info != NULL ? info->start_lba : 0;
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
	print("root: reading %d MB from 'darwin' (LBA %d, %d blocks of %d)\n", CHIRON_ROOT_BYTES >> 20,
	    first_lba, blocks, bs);
	started = READ_SYSREG(CNTVCT_EL0);
	for (done = 0; done < CHIRON_ROOT_BYTES; done += CHIRON_ROOT_CHUNK) {
		uint64_t want = CHIRON_ROOT_BYTES - done;

		if (want > CHIRON_ROOT_CHUNK) {
			want = CHIRON_ROOT_CHUNK;
		}
		/* ReadBlocks takes bytes, not blocks (see block_poll's use of the same protocol) */
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, done / bs, want, dest + done))) {
			print("root: read failed at block %d of darwin\n", done / bs);
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
