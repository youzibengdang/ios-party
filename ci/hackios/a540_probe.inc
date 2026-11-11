/*
 * ---- chiron: the Adreno 540 (a5xx), the first steps -------------------------------------------
 *
 * Everything the steps above do is for the Adreno 642L, whose power management processor is a
 * GMU with a firmware of its own. chiron's GPU is an earlier design: its power is switched by
 * two GDSCs that software drives directly, its clock tree is the msm8998 GPUCC, and its
 * microcode (pfp/pm4) is read out of a file instead of being handed to TrustZone. None of the
 * GMU's registers exist here, so nothing above can be reused past the clock and power stages.
 *
 * These steps are what comes first on that design: switch the outside clocks and the two
 * power switches, then read what the 3D core says about itself. Nothing is written beyond
 * those, nothing is loaded, no ring is made: if the core answers, the next steps have ground
 * to stand on; if it does not, the reason is in the lines this leaves.
 *
 *   chiron_gpu=1   steps 1 to 5
 *   chiron_gpu=2   ... and leave the core powered and clocked (the default stops it again)
 *   chiron_gpu=3   only read, no write at all: what the loader left the block as
 *
 * Addresses: chiron's device tree (gpu@5000000 "qcom,adreno-540.1", gpucc@5065000, the GCC
 * node at 0x100000), register meanings from Linux drivers/clk/qcom/{gcc,gpucc}-msm8998.c and
 * drivers/gpu/drm/msm/adreno/a5xx_gpu.c (a5xx_pm_resume, a5xx_hw_init).
 */
#define A540_GCC_BASE           0x00100000u
#define A540_GCC_SIZE           0x000b0000u
#define A540_GPUCC_BASE         0x05065000u
#define A540_GPUCC_SIZE         0x00009000u
#define A540_GPU_BASE           0x05000000u
#define A540_GPU_SIZE           0x00040000u

/* GCC: the clocks that feed the GPU block from outside (gcc-msm8998.c) */
#define A540_GCC_GPU_CFG_AHB    0x71004u        /* bit 0: the register bus of the whole GPU block */
#define A540_GCC_BIMC_GFX       0x46040u        /* bit 0: the core's path to the memory controller */
#define A540_GCC_GPU_BIMC_GFX   0x71010u        /* bit 0: the GPU's own side of that path */
#define A540_GCC_GPU_PLL_VOTES  0x5200cU        /* bit 4: GPLL0 to the GPU, bit 3: its divided tap */

/* GPUCC (gpucc-msm8998.c): two power switches and four clocks */
#define A540_GPUCC_CX_GDSCR     0x1004u         /* gpu_cx_gdsc: the half that is not the 3D core */
#define A540_GPUCC_CX_HW_CTRL   0x1008u
#define A540_GPUCC_CX_BCR       0x1000u
#define A540_GPUCC_CXO_CBCR     0x1020u
#define A540_GPUCC_RBCPR_CBCR   0x1054u
#define A540_GPUCC_RBCPR_RCGR   0x1030u
#define A540_GPUCC_GFX3D_RCGR   0x1070u         /* gfx3d_clk_src: source 1 = GPU PLL, /3 */
#define A540_GPUCC_GX_GDSCR     0x1094u         /* gpu_gx_gdsc: the 3D core */
#define A540_GPUCC_GX_BCR       0x1090u
#define A540_GPUCC_GX_CLAMP     0x0130u
#define A540_GPUCC_GFX3D_CBCR   0x1098u         /* bit 0: the clock; bit 13: the core's own logic */
#define A540_GPUCC_RBBMTIMER    0x10d0u
#define A540_GPUCC_GPUPLL0      0x0000u

/* a GDSC's control register (gdsc.c) */
#define A540_GDSC_SW_COLLAPSE   (1u << 0)       /* 1: the switch is open, the half has no power */
#define A540_GDSC_HW_CTRL       (1u << 1)       /* 1: something else drives the switch */
#define A540_GDSC_POWER_ON      (1u << 31)      /* 1: the half has power */

/* the 3D core's own first words: RBBM is at +0x2000 in this design, its ID registers lead it */
#define A540_RBBM_BASE          0x2000u
static uint32_t redwood_adreno_a540_probe(uint32_t through);
static vm_offset_t a540_gcc, a540_gpucc, a540_gpu;

/*
 * Whether this is chiron at all, and how far to go. The loader says where the peripherals begin:
 * msm8998 puts them at 0, the Adreno 642L phone at 0x400000, and on any other machine -- QEMU
 * above all -- not one register below is touched. "chiron_gpu=0" switches it off by hand,
 * "chiron_gpu=2" leaves the core powered, "chiron_gpu=3" only reads.
 */
static bool
redwood_adreno_a540_asked(uint32_t *through)
{
	uint32_t asked = 0;

	if (PE_parse_boot_argn("chiron_gpu", &asked, sizeof(asked))) {
		if (asked == 0) {
			printf("a540: chiron_gpu=0: the GPU is left alone\n");
			return false;
		}
		*through = asked;
		return true;
	}
	if (pe_arm_get_soc_base_phys() == 0) {
		*through = 1;
		return true;
	}
	return false;
}


static uint32_t
a540_rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static void
a540_wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

/* Read a register until the bits in mask equal want. The GPU block has no timer of ours to wait on. */
static bool
a540_wait(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t rounds)
{
	for (uint32_t i = 0; i < rounds; i++) {
		if ((a540_rd(base, offset) & mask) == want) {
			return true;
		}
		for (volatile uint32_t spin = 0; spin < 40; spin++) {
		}
	}
	return false;
}

/* A clock of either controller on, and whether its status bit says so (halt_check in Linux). */
static void
a540_clock(const char *block, vm_offset_t base, const char *name, uint32_t cbcr, uint32_t bits)
{
	uint32_t was = a540_rd(base, cbcr);
	uint32_t now;

	a540_wr(base, cbcr, was | bits);
	a540_wait(base, cbcr, 1u << 31, 0, 40);          /* bit 31: the branch says it is gated (clk-branch.c) */
	now = a540_rd(base, cbcr);
	printf("a540: %s %s: %#x -> %#x%s\n", block, name, was, now,
	    (now & (1u << 31)) ? " (still says off)" : " (on)");
}

static void
a540_dump_core(const char *when)
{
	uint32_t words[16];

	for (unsigned i = 0; i < 16; i++) {
		words[i] = a540_rd(a540_gpu, A540_RBBM_BASE + 4 * i);
	}
	printf("a540: the core's first words, %s:\n", when);
	for (unsigned row = 0; row < 4; row++) {
		printf("a540:   +%04x: %08x %08x %08x %08x\n", A540_RBBM_BASE + 16 * row, words[4 * row],
		    words[4 * row + 1], words[4 * row + 2], words[4 * row + 3]);
	}
}

/*
 * Steps: 1 map and look, 2 the outside clocks, 3 the CX half's power, 4 the 3D core's power
 * and clock, 5 read the core. `through` says how far to go; 0 means every step.
 */
static uint32_t
redwood_adreno_a540_probe(uint32_t through)
{
	uint32_t cx, gx;
	bool read_only = (through == 3);

	printf("\na540: chiron's GPU: an Adreno 540 at %#x, its clock controller at %#x, the global one at %#x\n",
	    A540_GPU_BASE, A540_GPUCC_BASE, A540_GCC_BASE);
	if (through == 3) {
		printf("a540: read only: not one write follows\n");
	}
	/* Step 1: the three windows, as the loader left them. */
	a540_gcc = ml_io_map(A540_GCC_BASE, A540_GCC_SIZE);
	a540_gpucc = ml_io_map(A540_GPUCC_BASE, A540_GPUCC_SIZE);
	a540_gpu = ml_io_map(A540_GPU_BASE, A540_GPU_SIZE);
	if (a540_gcc == 0 || a540_gpucc == 0 || a540_gpu == 0) {
		printf("a540: 1: a window would not map (gcc %#lx gpucc %#lx gpu %#lx): nothing done\n",
		    (unsigned long)a540_gcc, (unsigned long)a540_gpucc, (unsigned long)a540_gpu);
		return 1;
	}
	cx = a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR);
	gx = a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR);
	printf("a540: 1: as the loader left them: cx_gdscr %#x (%s), gx_gdscr %#x (%s), gfx3d_cbcr %#x\n", cx,
	    (cx & A540_GDSC_POWER_ON) ? "CX has power" : "CX has none", gx,
	    (gx & A540_GDSC_POWER_ON) ? "the core has power" : "the core has none",
	    a540_rd(a540_gpucc, A540_GPUCC_GFX3D_CBCR));
	printf("a540: 1: gcc: gpu_cfg_ahb %#x, gpu_bimc_gfx %#x, bimc_gfx %#x, gpu pll votes %#x\n",
	    a540_rd(a540_gcc, A540_GCC_GPU_CFG_AHB), a540_rd(a540_gcc, A540_GCC_GPU_BIMC_GFX),
	    a540_rd(a540_gcc, A540_GCC_BIMC_GFX), a540_rd(a540_gcc, A540_GCC_GPU_PLL_VOTES));
	if (read_only) {
		a540_dump_core("read only, with whatever power the loader left");
		return 1;
	}
	/* Step 2: the clocks outside the block. Without the register bus clock, nothing in it answers. */
	printf("a540: 2: the global clock controller: the GPU's register bus and its memory path on\n");
	a540_clock("gcc", a540_gcc, "gpu_cfg_ahb", A540_GCC_GPU_CFG_AHB, 1u << 0);
	a540_clock("gcc", a540_gcc, "gpu_bimc_gfx", A540_GCC_GPU_BIMC_GFX, 1u << 0);
	a540_clock("gcc", a540_gcc, "bimc_gfx", A540_GCC_BIMC_GFX, 1u << 0);
	a540_clock("gcc", a540_gcc, "gpu pll votes", A540_GCC_GPU_PLL_VOTES, (1u << 4) | (1u << 3));
	/* Step 3: the CX half: the SMMU and the bus interface live there, and the core's power hangs off it. */
	printf("a540: 3: cx_gdscr %#x: clear its collapse bit and wait for the power-on bit\n",
	    a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR));
	a540_wr(a540_gpucc, A540_GPUCC_CX_GDSCR, a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR) & ~A540_GDSC_SW_COLLAPSE);
	if (!a540_wait(a540_gpucc, A540_GPUCC_CX_GDSCR, A540_GDSC_POWER_ON, A540_GDSC_POWER_ON, 400)) {
		cx = a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR);
		printf("a540: 3: the CX half does not come on (cx_gdscr %#x, hw_ctrl %#x): the supply it needs (VDDMX, "
		    "from RPM) is not there, or something else drives the switch\n", cx,
		    a540_rd(a540_gpucc, A540_GPUCC_CX_HW_CTRL));
		return 3;
	}
	printf("a540: 3: CX has power\n");
	/* Step 4: the core itself. Its clock is the GPU PLL divided by three, which the loader may have left running. */
	printf("a540: 4: gfx3d_clk_src %#x, gpupll0 mode %#x: the core's clock comes from the GPU PLL\n",
	    a540_rd(a540_gpucc, A540_GPUCC_GFX3D_RCGR + 4), a540_rd(a540_gpucc, A540_GPUCC_GPUPLL0 + 8));
	a540_clock("gpucc", a540_gpucc, "cxo", A540_GPUCC_CXO_CBCR, 1u << 0);
	a540_clock("gpucc", a540_gpucc, "rbbmtimer", A540_GPUCC_RBBMTIMER, 1u << 0);
	a540_clock("gpucc", a540_gpucc, "rbcpr", A540_GPUCC_RBCPR_CBCR, 1u << 0);
	printf("a540: 4: gx_gdscr %#x: the 3D core's power switch\n", a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR));
	a540_wr(a540_gpucc, A540_GPUCC_GX_GDSCR, a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR) & ~A540_GDSC_SW_COLLAPSE);
	if (!a540_wait(a540_gpucc, A540_GPUCC_GX_GDSCR, A540_GDSC_POWER_ON, A540_GDSC_POWER_ON, 400)) {
		printf("a540: 4: the core does not come on (gx_gdscr %#x, clamp %#x)\n",
		    a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR), a540_rd(a540_gpucc, A540_GPUCC_GX_CLAMP));
		return 4;
	}
	printf("a540: 4: the core has power\n");
	a540_clock("gpucc", a540_gpucc, "gfx3d", A540_GPUCC_GFX3D_CBCR, (1u << 0) | (1u << 13));
	/* Step 5: what the core says of itself, now that all of it is fed. */
	a540_dump_core("with power and clocks");
	printf("a540: 5: gfx3d_cbcr %#x, rcgr %#x, cx %#x, gx %#x\n", a540_rd(a540_gpucc, A540_GPUCC_GFX3D_CBCR),
	    a540_rd(a540_gpucc, A540_GPUCC_GFX3D_RCGR + 4), a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR),
	    a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR));
	if (through == 2) {
		printf("a540: 5: left powered and clocked, as asked\n");
		return 0;
	}
	printf("a540: 5: switching the core and then the CX half off again (the loader's own state is not restored)\n");
	a540_wr(a540_gpucc, A540_GPUCC_GFX3D_CBCR, a540_rd(a540_gpucc, A540_GPUCC_GFX3D_CBCR) & ~(1u << 0));
	a540_wr(a540_gpucc, A540_GPUCC_GX_GDSCR, a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR) | A540_GDSC_SW_COLLAPSE);
	a540_wait(a540_gpucc, A540_GPUCC_GX_GDSCR, A540_GDSC_POWER_ON, 0, 400);
	a540_wr(a540_gpucc, A540_GPUCC_CX_GDSCR, a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR) | A540_GDSC_SW_COLLAPSE);
	a540_wait(a540_gpucc, A540_GPUCC_CX_GDSCR, A540_GDSC_POWER_ON, 0, 400);
	printf("a540: 5: done: gx %#x, cx %#x\n", a540_rd(a540_gpucc, A540_GPUCC_GX_GDSCR),
	    a540_rd(a540_gpucc, A540_GPUCC_CX_GDSCR));
	return 0;
}
