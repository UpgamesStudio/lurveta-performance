# Rootfs path relocation check

`RelocTest.java` runs the app's real `PathRelocator` (compile it together with
`app/src/main/java/com/winlator/core/PathRelocator.java` plus tiny stubs for
`android.util.Log`, `com.winlator.BuildConfig` and `com.winlator.core.StreamUtils`)
over an uncompressed `.tar` of an asset, mirroring `TarCompressorUtils.extract()`.

`verify_reloc.py <original_extract> <relocated_extract>` then checks that:
- no file contains `com.winlator` any more;
- every changed file differs only by the 36-byte prefix replacement;
- symlink targets were relocated.

`scan_paths.py <dirs...>` is the audit script that found that 100% of the occurrences
share the prefix `/data/data/com.winlator/files/rootfs`.
