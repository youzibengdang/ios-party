/*
 * ---- chiron: the iOS root volume kept in a UFS partition --------------------------------
 *
 * "oem ramdisk" makes the host send a root image over USB into the resident area. A 2.4 GB
 * volume takes 73 s, and the loader forgets it on every reset, so a kernel iteration would
 * pay that again and again. This command reads the same volume out of a flash partition
 * instead: the host writes it once (fastboot flash system, from the stock bootloader), and
 * from then on the loader fills the resident area from the storage in about a quarter of a
 * minute, with no USB traffic at all.
 *
 * The size is fixed here on purpose: it is one image, whose identity the project records
 * (system.apfs, 2,492,219,392 bytes, sha256 f781f611...f65fa4). A partition holding exactly
 * that prefix is all this needs; nothing beyond it is touched.
 */
#define CHIRON_ROOT_NAME    u"system"
#define CHIRON_ROOT_BYTES   2492219392ULL
#define CHIRON_ROOT_CHUNK   (8ULL << 20)

/* The partition the root volume was written to, found the same way the log partition is. */
static efi_block_io *
chiron_root_partition(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	static const char16 want[] = CHIRON_ROOT_NAME;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/* Fill the resident area from the partition. 1 = the resident root is in place. */
static int
chiron_root_from_ufs(void)
{
	struct payload_info pi;
	efi_block_io *bio;
	uint64_t bs, blocks, done, started, limit;
	uint8_t *dest;

	bio = chiron_root_partition();
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
	print("root: reading %d MB from 'system' at %d bytes per block\n", CHIRON_ROOT_BYTES >> 20, bs);
	started = READ_SYSREG(CNTVCT_EL0);
	for (done = 0; done < CHIRON_ROOT_BYTES; ) {
		uint64_t want = CHIRON_ROOT_BYTES - done;
		uint64_t lba = done / bs;

		if (want > CHIRON_ROOT_CHUNK) {
			want = CHIRON_ROOT_CHUNK;
		}
		if (want % bs != 0) {
			want = (want / bs) * bs;              /* a whole number of blocks, the tail is finished below */
			if (want == 0) {
				want = bs;
			}
		}
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, lba, want / bs, dest + done))) {
			print("root: read failed at block %d\n", lba);
			return 0;
		}
		done += want;
		if ((done >> 20) % 256 == 0) {
			print("root: %d of %d MB\n", done >> 20, CHIRON_ROOT_BYTES >> 20);
		}
	}
	clean_dcache((uint64_t)(uintptr_t)dest, (uint64_t)(uintptr_t)dest + CHIRON_ROOT_BYTES);
	resident_rd_size = CHIRON_ROOT_BYTES;
	print("root: %d MB in place in %d ms, the partition begins %c%c%c%c\n", CHIRON_ROOT_BYTES >> 20,
	    (READ_SYSREG(CNTVCT_EL0) - started) / 19200, dest[32], dest[33], dest[34], dest[35]);
	return 1;
}
