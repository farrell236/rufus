Bundled UEFI Shell payloads
===========================

These release-built UEFI Shell 2.2 executables come from:
https://github.com/pbatard/UEFI-Shell

The five current architectures use release 26H1, commit ae26317, built from
TianoCore edk2-stable202602 (commit b7a715f7c03c45c6b4575bf88596bfd79658b8ce):

1569b6db4e391c3c59194aa3319a3945efb800fb25349eb9d36ff3d258517ea6  shellaa64.efi
54ae3a8f58b6fe7123fd948d0773c88e8c26834e39acd3874732c96cbe7c0dd5  shellia32.efi
d6c97ae52707ebbad4eda063cb0aefc467ec942b07461a6d6d1119cad0ac3e9c  shellloongarch64.efi
ccdb9523276d470277f7676d6534916534cd70218ea5c4cc5ac302e149f65196  shellriscv64.efi
4ea080ddd576117cd04f5c02d16712ea5d9249c0752214d8e4055e460d7b11e0  shellx64.efi

TianoCore removed ARM32 support after 25H1. The ARM32 executable is retained
from its final supported release, 25H1, commit 83fde42, built from
edk2-stable202505:

eef9c4908b634d9fe0c853c75c284666319058b07545f03f9a7b8303a390a83f  shellarm.efi

The application verifies these SHA-256 digests and the PE architecture and
UEFI-application subsystem before staging a disk image. These executables are
not Microsoft-signed; Secure Boot must be disabled to boot them.

See License.txt in this directory for the BSD-2-Clause-Patent terms.
