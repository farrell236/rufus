Secure Boot revocation data
===========================

The bundled DBX files are from Microsoft's secureboot_objects signed release
v1.7.0-signed, published 2026-09-03:

https://github.com/microsoft/secureboot_objects/releases/tag/v1.7.0-signed

The signed release archive covers x86-64, x86-32, ARM32, and ARM64. The
application verifies the embedded archive against Microsoft's published
SHA-256 digest before validating and parsing its ZIP and DBX contents.

The optional in-app update check is user initiated. It queries the official
Microsoft GitHub release feed, asks before downloading a newer signed archive,
checks the archive against the SHA-256 digest in the release metadata, validates
its ZIP structure and DBX contents, and only then stores it in the application
data directory. The embedded release remains the offline fallback.

The Microsoft material is redistributed under License.txt in this directory.
