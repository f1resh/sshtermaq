# SSHTermaq's vcpkg triplet: everything is linked statically, including the C/C++ runtime (/MT), so the app
# needs no Visual C++ Redistributable. libssh is the one exception: it stays a DLL (ssh.dll, with its own static
# runtime) so it can be replaced under the terms of the LGPL. Memory allocated by libssh must therefore always be
# released with libssh's own functions (ssh_string_free_char, ssh_key_free, ...), never with free().
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
# Exploit mitigations (1.2.0 review item 11.6), as for the program itself (CMakeLists.txt): Control Flow Guard in every
# package but OpenSSL (below), and CET shadow-stack compatibility for what is linked (ssh.dll).
set(VCPKG_C_FLAGS "/guard:cf")
set(VCPKG_CXX_FLAGS "/guard:cf")
set(VCPKG_LINKER_FLAGS "/guard:cf /CETCOMPAT")

# OpenSSL's C code calls its assembly routines (AES-GCM, ChaCha20-Poly1305, ...) through function pointers, and
# assembly carries no CFG metadata: compiled with /guard:cf, those calls fail the check and the process ends
# (FAST_FAIL_GUARD_ICALL_CHECK_FAILURE, seen 2026-11-24). So OpenSSL's own code is compiled without CFG checks; what
# links it (ssh.dll, SSHTermaq.exe) is still CFG and CET marked, and everything else in them is checked.
if(PORT STREQUAL "openssl")
    set(VCPKG_C_FLAGS "")
    set(VCPKG_CXX_FLAGS "")
endif()

if(PORT STREQUAL "libssh")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
    # ssh.dll's own imports (system DLLs) from its folder and System32 only, never the current folder or PATH.
    set(VCPKG_LINKER_FLAGS "${VCPKG_LINKER_FLAGS} /DEPENDENTLOADFLAG:0xA00")
endif()
