#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the sweet live FDT. Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--spacewar] [--ft-fw FILE.i]
--spacewar: Nothing Phone (1) (SM7325): RAM, framebuffer and vtimer from its live FDT and probes 1-7.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: above every Qualcomm XPU carve-out (see NOTES.md)
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the whole upper DDR bank. The live FDT memory node is
# <0x80000000 2G>, <0x1_00000000 0xfcc00000>; every fixed reserved-memory
# carve-out is in the low bank and the dynamic pools are limited below 4G.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = 0xfcc00000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0x1700000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2400, 1080 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27
# Spacewar (NOTES.md "Second port"): no firmware carve-out from 0xE3400000 to the
# end of RAM (0x2_80000000); ABL's splash buffer (VIG0 scanout, refreshed by MDP
# on its own) at 0xE1000000; standard timer PPIs (vtimer INTID 27).
SPACEWAR_PHYS_BASE, SPACEWAR_MEM_SIZE = 0xe4000000, 0xfc000000
SPACEWAR_FB = 0xe1000000
# UFS VCC (flash supply): live DT ufshc vcc-supply = pm7325_l7 "ldob7",
# vcc-voltage-level <2504000 2952000>. ABL leaves it unusable (media reads hang).
SPACEWAR_UFS_VCC, SPACEWAR_UFS_VCC_MV = 'ldob7', 2952
SPACEWAR_CMD_DB = (0x80860000, 0x20000)  # reserved-memory cmd_db, qcom,cmd-db
# Keys (bsd/dev/sweet_keys.c): power/volume-down on PMK8350 SID 0 PON gen3
# (pon_hlos@1300, RT_STS 0x1310: KPDPWR bit 7, RESIN bit 6); volume-up on
# PM7325 (SID 1) GPIO6 = peripheral 0x8d, active low.
SPACEWAR_KEYS = dict(pon_ppid=0x013, pon_rt=0x1310, pwr_mask=0x80, down_mask=0x40,
                     gpio_ppid=0x18d, gpio_base=0x8d00)
# RTC (SweetPlatformExpert): PMK8350 rtc@6100 counts seconds from an arbitrary
# base; real time = RTC + Qualcomm time_daemon's offset from persist
# time/ats_2 (ms), as backed up on 2026-10-19 (valid until the RTC resets).
SPACEWAR_RTC_BASE, SPACEWAR_RTC_OFFSET_MS = 0x6100, 0x01a15422972b
# CPU frequency (bsd/dev/sweet_cpufreq.c): EPSS cpufreq-hw domains (live DT
# qcom,cpufreq-hw-epss freq-domain0/1/2 = A55 / A78 / prime). ABL leaves the
# A78 and prime domains at LUT row 0 (691 / 806 MHz); the kernel requests the top row.
SPACEWAR_EPSS = (0x18591000, 0x1000, 0x18592000, 0x1000, 0x18593000, 0x1000)
# USB (bsd/dev/sweet_usb.c): SM7325 gcc-yupik offsets differ from sweet's
# gcc-sdmmagpie, the HS PHY is snps-femto (no QUSB2 block), and the DWC3 is
# behind apps_smmu stream 0xe0 (live DT ssusb iommus <0x1a 0xe0>).
# offsets from the GCC base 0x100000 (gcc-yupik.c halt_reg / gdsc@10f004 -> 0xf004)
SPACEWAR_USB = dict(gdscr_off=0x0f004, master_off=0x0f010, cfgnoc_off=0x0f07c, smmu_sid=0xe0)

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-x/d22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

def ft_firmware(path):
    """Focaltech app .i (a C initializer list of 0xNN bytes) -> bytes.
    Kept out of git; byte-identical to the blob in the phone's fts_tp.ko."""
    import re
    data = open(path).read()
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{1,2})\b', data))


u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, spacewar=False, ft_fw=None):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    fb, vtimer = FB, VTIMER_INTID
    if spacewar:
        phys_base, mem_size, fb, vtimer = SPACEWAR_PHYS_BASE, SPACEWAR_MEM_SIZE, SPACEWAR_FB, QEMU_VTIMER_INTID
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SWEET SMP: all eight cores in physical-core order (0-5 Cortex-A55,
    # 6-7 Cortex-A76), one homogeneous cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2304000000 if core >= 6 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else vtimer)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(fb, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ] + ([('vcc-rpmh', cstr(SPACEWAR_UFS_VCC)), ('vcc-mv', u32(SPACEWAR_UFS_VCC_MV))] if spacewar else []))
    # bsd/dev/sweet_rpmh.c: cmd-db without the AOP locator (Spacewar).
    rpmh = [node('sweet-rpmh', [ph(), ('cmd-db', u64(*SPACEWAR_CMD_DB))]),
            node('sweet-keys', [ph()] + [(k.replace('_', '-'), u32(v)) for k, v in SPACEWAR_KEYS.items()]),
            node('sweet-rtc', [ph(), ('base', u32(SPACEWAR_RTC_BASE)),
                               ('offset-ms', u64(SPACEWAR_RTC_OFFSET_MS))])] if spacewar else []
    # bsd/dev/sweet_fts_spi.c: Focaltech FT3680 on SE13 SPI via GPI DMA. The
    # firmware blob (downloaded to PRAM every boot) rides in this node when
    # --ft-fw is given; absent, the driver reports the boot id only.
    if spacewar:
        fts_props = [ph()]
        if ft_fw:
            fts_props.append(('firmware', ft_fw))
        rpmh.append(node('sweet-fts-spi', fts_props))
        rpmh.append(node('sweet-cpufreq', [ph(), ('reg', u64(*SPACEWAR_EPSS))]))
        rpmh.append(node('sweet-usb', [ph(), ('no-qusb2', u32(1))] +
                         [(k.replace('_', '-'), u32(v)) for k, v in SPACEWAR_USB.items()]))

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = d22ap_product()
    if product_props:
        # D22AP says P3: MobileGestalt HasExtendedColorDisplay = gamut == "P3", and
        # WebKit then renders into 10-bit IOSurfaces ('b3a8'/'w30r') that the
        # CPU renderer cannot draw; Safari showed no page content. sRGB = 8-bit BGRA.
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v)
                         for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('qcom,sm7150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]) + rpmh)


if __name__ == '__main__':
    ft_fw = None
    if '--ft-fw' in sys.argv:
        ft_fw = ft_firmware(sys.argv[sys.argv.index('--ft-fw') + 1])
        print(f'FT3680 firmware: {len(ft_fw)} bytes', file=sys.stderr)
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv,
                 spacewar='--spacewar' in sys.argv, ft_fw=ft_fw)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
