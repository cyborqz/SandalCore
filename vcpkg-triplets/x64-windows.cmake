# Triplet for the libraries that vcpkg builds for this project.
#
# It is vcpkg's own x64-windows triplet, with one addition: the platform
# toolset. vcpkg otherwise picks the newest Visual Studio on the machine - on a
# machine with both Visual Studio 2022 and 2026 that is the 2026 one (toolset
# v145, STL 14.52), while the project itself is built with the "Visual Studio
# 17 2022" generator (toolset v143, STL 14.44). The two are not
# link-compatible: the libraries then reference STL internals that the
# program's STL does not have, and the link fails with
# "unresolved external symbol __std_find_first_not_of_trivial_pos_1" and friends.
#
# Pinning the toolset here builds the libraries with the same compiler and the
# same STL as the programs that link them.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PLATFORM_TOOLSET v143)
