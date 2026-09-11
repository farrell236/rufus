# Features

This document describes the implemented Rufus++ feature surface, platform
availability, optional dependencies, and known limitations. For the internal
architecture and device-safety boundary, see
[cross-platform.md](cross-platform.md).

> [!WARNING]
> Rufus++ remains a development prototype. Destructive operations are
> confirmation-gated, but release signing and real removable-hardware
> qualification are not complete.

## At a glance

- Native Qt interface and CMake build on Windows, Linux, and macOS.
- Read-only discovery of whole physical devices, with protected devices hidden
  by default.
- ISO, raw, VHD, VHDX, FFU, and compressed-image analysis and deployment.
- Windows installation, Windows To Go, Linux persistence, and macOS installer
  workflows selected from detected source capabilities.
- Standalone formatting, blank boot media, drive capture, checksums,
  verification, bad-block testing, and read-only device inspection.
- Structured preflight reports and JSON receipts for destructive operations.
- Offline UEFI Shell payloads, runtime UEFI validation, and Secure Boot
  analysis without automatic component downloads.

## Source analysis

Rufus++ reads bounded ISO-9660, Joliet, Rock Ridge, and ordinary UDF directory
trees. It validates El Torito boot catalogs, MBR and GPT structures, WIM
header/XML metadata, VHD/VHDX/FFU containers, and compressed-image sizing
metadata.

The analyzer detects:

- Windows installer media and available WIM/ESD editions
- Linux live bootloaders and supported persistence styles
- BIOS and UEFI boot capability
- CPU architecture
- Hybrid ISO/raw capability
- Files larger than the FAT32 limit
- Fixed and parentless dynamic VHD/VHDX layouts
- Structurally valid FFU images

Differencing virtual disks, VHDX images with active logs, and unsupported UDF
partition maps remain inspection-only rather than exposing an unsafe or
incomplete write operation.

## Image deployment

### Raw and compressed images

Raw DD deployment uses immutable write plans, source revalidation,
cancellation, synchronized writes, explicit flushing, verification, and
partial-write reporting.

Gzip, Bzip2, single-image ZIP, LZMA-alone, XZ, and Zstandard images use the same
streaming write engine without creating a second full-size image. Preflight
resolves and validates the expanded size before enabling START. ZIP support is
limited to one stored or Deflate-compressed regular file; encrypted, ZIP64,
split, multi-image, and unsupported-method archives are rejected.

Validated fixed VHDs deploy without copying the container footer. Parentless
dynamic VHD/VHDX images are reconstructed from their sparse allocation tables
while writing and verifying.

### ISO mode

The portable ISO stager converts readable ISO-9660, Joliet, or ordinary UDF
trees into a validated MBR or GPT FAT32 disk image. It preserves long
filenames, byte-verifies staged files, and commits the result through the native
guarded writer.

- GPT is constrained to UEFI targets.
- MBR supports validated BIOS, UEFI, and dual-target layouts.
- Standard GRUB2 BIOS trees receive an embedded MBR/core image in the
  pre-partition gap.
- Alternate supported GRUB directory layouts are aliased to the bootstrap's
  expected path.

NTFS/UEFI:NTFS ISO mode is dependency-gated. It creates an MBR or GPT NTFS data
partition plus the embedded UEFI:NTFS helper partition, extracts and verifies
every source file, and sends the resulting disk image to the native writer.

### Windows installation media

Standard Windows Setup deployment supports FAT32 and UEFI. On 512-byte-sector
targets, BIOS-capable images also receive an active Windows-compatible MBR and
BOOTMGR FAT32 bootstrap.

When `sources/install.wim` or `sources/install.esd` exceeds the FAT32 file-size
limit, wimlib converts solid ESD resources when necessary and creates validated
`install.swm`, `install2.swm`, and subsequent parts. The same path is used on
all three host operating systems.

Optional, credential-free Windows User Experience settings can be embedded in
`autounattend.xml`, including supported hardware-check bypasses, local-account,
regional, privacy, encryption, and consumer-content choices.

### Windows To Go

Windows To Go is exposed only when the source and complete host toolchain are
compatible. The UI reads the actual WIM/ESD edition list and generates a
credential-free `unattend.xml` from the selected options.

The stager creates a sparse GPT image with EFI, Microsoft Reserved, and NTFS
partitions, applies the selected edition with wimlib, constructs Windows Boot
Manager/BCD data, and commits the result through the native raw writer.

- Windows uses an attachable fixed-VHD staging view, DiskPart, and BCDBoot.
- Linux and macOS use loop/disk-image attachment and BCD-SYS.
- The source ISO is revalidated and never modified.

### Linux persistence

Rufus++ does not infer persistence support from a distribution name or from the
mere presence of GRUB or Syslinux. It inspects bounded, recognized boot
configuration files and exposes the persistence-size control only when an
actual Casper or Debian Live kernel entry can be patched safely. Unknown live
layouts remain available for ordinary ISO or exact DD deployment without a
persistence control. The available persistence size is calculated from target
capacity after reserving 110% of the extracted ISO payload.

The portable stager creates a second MBR partition containing a verified ext2
filesystem:

- Ubuntu/Casper media uses the `casper-rw` label.
- Debian-style media uses the `persistence` label and `/persistence.conf`.
- Recognized GRUB and Syslinux entries are patched idempotently.
- Affected MD5 manifests are updated before the complete image is written.

Custom persistence implementations are not treated as generic Debian Live
media. In particular, Tails retains its native encrypted `TailsData` workflow,
and Pop!_OS media carrying its custom Casper layout does not expose this
control.

Legacy-BIOS installation for Syslinux-only media remains capability-gated.
Hybrid images can still use exact DD mode.

### macOS installers

On macOS, an Apple-signed `Install macOS ….app` can be selected as a distinct
source. Rufus++ validates the bundle and delegates bootable-installer creation
to its native `Contents/Resources/createinstallmedia` executable.

GPT, Mac firmware, filesystem, cluster size, and post-write controls are fixed
to the Apple-managed workflow. Quick format uses Apple's normal erase path;
clearing it adds a complete zero-fill and read-back verification pass before
preparing a temporary Journaled HFS+ volume.

The signed helper revalidates the application signature, tool interface,
target identity, and capacity, launches fixed executables without a shell,
relays progress, and supports cancellation. An explicitly compiled unsigned
root development mode performs the same checks and local operation in the
whole elevated process without packaging the helper. This creates macOS
installation media, not a live or portable macOS system.

### FFU

Windows applies validated FFU images through DISM. It locks the source and
target, prevents deployment from a source stored on the target, dismounts
target volumes, revalidates the physical drive, supports cancellation, and
refreshes the disk layout afterward.

FFU application remains analysis-only on Linux and macOS because those systems
do not provide the Windows servicing stack.

## Formatting and blank media

Standalone formatting is confirmation-gated. FAT16, FAT32, and ext2 staging are
host-independent. NTFS, exFAT, UDF, ext3, and Windows ReFS appear only when the
host has a compatible provider.

Cluster sizes are validated for the selected filesystem. Portable quick FAT
formats preserve otherwise-unused sectors; full format overwrites and verifies
them. Installed NTFS providers receive their native quick/full choice.

Blank boot-media options include:

- Bundled FreeDOS media
- MS-DOS 7/8 from user-selected system files
- Embedded GRUB2 and Syslinux 4.07 prompts
- User-supplied Grub4DOS, ReactOS, and UEFI loaders
- Dependency-gated UEFI:NTFS
- Offline UEFI Shell 2.2 for one or every bundled architecture

UEFI Shell releases and architectures are pinned, and each payload is checked
against its published SHA-256 and PE architecture before staging. Boot
components are not downloaded at runtime.

## Capture, inspection, and verification

### Drive capture

All hosts can capture an eligible removable device as verified DD, fixed VHD,
dynamic VHD, or dynamic VHDX. Windows additionally captures FFU through DISM.

Filesystem-aware UDF ISO capture requires exactly one mounted source volume and
records its file tree rather than its partition layout. It uses `hdiutil` on
macOS, `genisoimage` or `mkisofs` on Linux, and a user-installed `oscdimg.exe`
on Windows.

### Checksums and verification

MD5, SHA-1, SHA-256, and SHA-512 are calculated together in one cancellable
pass. Results are rejected if the source changes during calculation.

Deployment offers three verification profiles:

- **Fast:** deterministic beginning, end, and distributed payload samples.
- **Standard:** rereads and compares every written payload byte.
- **Full:** verifies the complete target and therefore requires a complete-disk
  image or full-format operation.

### Device inspection and testing

Capability Health reports native backend guarantees and optional host
dependencies. Its read-only media inspector samples the beginning and end of a
device to identify MBR/GPT integrity, partitions, boot indicators, and common
filesystems without mounting it.

Optional destructive bad-block/fake-capacity testing performs address-dependent
multi-pass writes and reads before deployment. Mismatches report sector counts
and initial byte offsets.

An optional QEMU smoke test launches supported ISO and raw images read-only. It
uses OVMF for compatible UEFI tests and observes the VM for a bounded interval;
it is not a substitute for real hardware validation.

## Secure Boot and UEFI

Offline Secure Boot analysis validates PE bounds, detects Authenticode tables,
calculates exact SHA-256 hashes, and applies SBAT generation floors. It starts
from Microsoft's packaged, signed `secureboot_objects` DBX release for x86-64,
x86-32, ARM32, and ARM64.

The data version, publication date, origin, and integrity state appear in About
and Capability Health. A user-initiated update check can offer a newer official
signed release and verifies its published SHA-256 before caching it. No update
is downloaded automatically. User-supplied text rules and EFI DBX
signature-list/authenticated-variable exports are supported as clearly marked
custom overlays.

Supported UEFI fallback loaders can optionally be wrapped with packaged runtime
validation applications and a complete MD5 manifest for offline validation at
boot.

Full Authenticode trust-chain evaluation, certificate-based DBX matching, and
Windows SVN analysis are not yet implemented.

## Safety and operation records

Before every destructive operation, Rufus++ displays a structured compatibility
report containing the source, stable target identity, layout, firmware,
filesystem, allocation unit, quick/full policy, transformations, dependencies,
warnings, and blockers.

Completed, cancelled, and failed operations produce an atomic JSON receipt in
the platform application-data directory. It includes bytes written and
verified, operation status, and the source SHA-256 if it was calculated for the
current image.

Long operations inhibit host sleep and use a progress-stall watchdog without
forcibly interrupting an in-flight device write.

Native device transports provide the following safeguards:

- **macOS:** Disk Arbitration claims and whole-disk unmounting, raw geometry
  revalidation, synchronized I/O, and a mutually authenticated `SMAppService`
  helper over XPC. Ordinary unsigned builds cannot write physical disks. An
  opt-in, visibly labelled unsigned-root development build omits the helper and
  enables the same local operations only when the process has an effective UID
  of zero. Its bottom-right runtime indicator distinguishes `UNPRIVILEGED` and
  `ELEVATED`; a signed-helper build displays `PRIVILEGED HELPER` only when that
  transport is registered and available. Privilege state is not added to the
  window title.
- **Linux:** non-forced unmounts, an exclusive block-device handle, kernel
  geometry revalidation, cache flushing, verification, and partition-table
  rereading. The application currently requires a trusted administrator
  launch.
- **Windows:** locks and dismounts every target-backed volume, revalidates the
  PhysicalDrive and geometry, uses write-through I/O, and refreshes the disk
  layout. AUTHORIZE opens a new UAC-elevated application window; the original
  window remains `UNPRIVILEGED` and the new process reports `ELEVATED`.

Every backend rejects a source image stored on its target and revalidates target
identity immediately before destructive access.

## Platform operation matrix

| Operation | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Raw/DD and compressed images | Signed helper or development root process | Elevated process | Elevated process |
| Apple installer application | `createinstallmedia` through signed helper or development root process | Unavailable | Unavailable |
| FAT32 ISO and Windows Setup | Portable stager + native writer | Portable stager + native writer | Portable stager + native writer |
| NTFS/UEFI:NTFS ISO | Host tools | Host tools, elevated | DiskPart, elevated |
| Linux persistence | Portable stager + native writer | Portable stager + native writer | Portable stager + native writer |
| Windows To Go | wimlib + host tools + BCD-SYS | wimlib + host tools + BCD-SYS | wimlib + DiskPart + BCDBoot |
| FFU apply/capture | Analysis only | Analysis only | DISM, elevated |
| DD/VHD/VHDX capture | Signed helper or development root process | Elevated process | Elevated process |
| UDF capture | `hdiutil` | `genisoimage`/`mkisofs` | User-installed `oscdimg.exe` |
| FAT16/FAT32/ext2 format | Portable stager + native writer | Portable stager + native writer | Portable stager + native writer |
| Host filesystem formats | exFAT/UDF; optional NTFS/ext3 | Installed tools, elevated | DiskPart, elevated |
| Bad-block testing | Signed helper or development root process | Elevated process | Elevated process |

Dependency-gated operations remain disabled until their complete provider
reports available. They do not fall back to a similarly named but incomplete
operation.

## Build requirements

The base Qt application requires:

- CMake 3.24 or later
- A C++17 compiler
- Qt 6.5 or later with Widgets and Network
- zlib, libbz2, liblzma, and libzstd development libraries

### Optional runtime dependencies

- **Large Windows Setup images:** wimlib 1.13.4 or later for WIM/ESD conversion
  and splitting.
- **Windows To Go:** wimlib 1.13.4 or later. Windows uses built-in DiskPart and
  BCDBoot. Linux additionally needs `losetup`, `mkfs.fat`, `mkntfs`, `ntfs-3g`,
  `mount`, BCD-SYS, `hivexsh`, `hivexregedit`, `peres`, `xxd`, `setfattr`, and
  `fatattr`. macOS needs `hdiutil`, `diskutil`, `newfs_msdos`, `mkntfs`,
  `ntfs-3g`, BCD-SYS, `hivexsh`, `hivexregedit`, `peres`, and `xxd`.
- **NTFS/UEFI:NTFS ISO:** Windows uses built-in DiskPart. Linux needs
  `losetup`, `mkntfs`, `ntfs-3g`, and `umount`. macOS needs `hdiutil`, `mkntfs`,
  `ntfs-3g`, and `umount`.
- **FFU:** Windows DISM with `/Apply-Ffu` or `/Capture-Ffu`.
- **macOS installer media:** macOS 13 or later, a complete Apple-signed
  installer containing `createinstallmedia`, a signed and registered Rufus++
  helper (or an explicitly compiled development root-mode app), and a target of
  at least 16 GiB. A 32 GB target is recommended.
- **Host filesystem formats:** macOS uses `newfs_exfat`/`newfs_udf`, with
  optional `mkntfs` and `mke2fs`; Linux uses `losetup` plus the relevant
  `mkntfs`, `mkfs.exfat`, `mkudffs`, or `mkfs.ext3`; Windows uses DiskPart.
- **UDF capture:** `hdiutil` on macOS, `genisoimage`/`mkisofs` on Linux, or a
  user-installed Windows ADK `oscdimg.exe` beside the application.
- **Virtual boot smoke testing:** an architecture-compatible QEMU system
  emulator and, for supported x86 UEFI tests, an OVMF code image.

[BCD-SYS](https://github.com/jpz4085/BCD-SYS) is never downloaded automatically.
Rufus++ likewise does not download Windows ISOs or Apple installer
applications.

## Known release gaps

- Automated portable archives and macOS DMGs are available, but production
  release signing and notarization are not yet configured.
- Release macOS app/helper builds must be signed with a matching Apple Team ID
  before physical writes are enabled. The opt-in unsigned-root mode is limited
  to local development and hardware testing.
- Linux and Windows still use whole-process elevation rather than dedicated
  least-privilege helpers.
- Cross-platform removable-hardware and fault-injection qualification remains
  outstanding.
- Windows To Go, NTFS ISO mode, and host-specific formats are unavailable when
  their complete external toolchains are missing.
- Legacy Syslinux-only BIOS media, uncommon UDF layouts, differencing VHD
  chains, and active-log VHDX images are deliberately restricted.
- QEMU validation is a smoke test, not proof that an image reaches an expected
  boot screen.

Portable-core, backend, platform-discovery, and headless Qt tests are available
through CTest.
