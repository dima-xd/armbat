add_library(armbat_core STATIC
    src/armbat/version.cpp
    src/bionic/bionic_boot.cpp
    src/bionic/guest_sysroot.cpp
    src/cpu/guest_arena.cpp
    src/cpu/a32_machine.cpp
    src/heap/guest_heap.cpp
    src/hle/hle.cpp
    src/hle/guest_stdio.cpp
    src/hle/guest_pthread.cpp
    src/hle/guest_libc.cpp
    src/hle/guest_extra.cpp
    src/hle/guest_android.cpp
    src/hle/guest_opensles.cpp
    src/hle/guest_input.cpp
    src/hle/guest_gles.cpp
    src/kernel/guest_kernel.cpp
    src/kernel/guest_signal.cpp
    src/kernel/guest_memory.cpp
    src/kernel/guest_modules.cpp
    src/loader/dynamic_loader.cpp
    src/loader/elf32_loader.cpp
    src/loader/guest_apk.cpp
    src/loader/relocations.cpp
    src/jni/jni_bridge.cpp
    src/runtime/crash_handler.cpp
    src/runtime/handles.cpp
    src/runtime/log.cpp
    src/runtime/symbolizer.cpp
    src/runtime/guest_container.cpp
)

# Gradle passes the APK's versionName/versionCode so the number has one owner;
# the defaults keep a bare `cmake --preset arm64` build working.
set(ARMBAT_VERSION_NAME "0.0.0" CACHE STRING "Version name reported by the runtime")
set(ARMBAT_VERSION_CODE 0 CACHE STRING "Version code reported by the runtime")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/armbat")
configure_file(version.h.in "${CMAKE_BINARY_DIR}/generated/armbat/version_config.h" @ONLY)

target_include_directories(armbat_core PUBLIC include "${CMAKE_BINARY_DIR}/generated")
# Dynarmic's headers are part of our own public headers; z/log/OpenSLES are
# implementation details and must not leak into every consumer.
target_link_libraries(armbat_core PUBLIC dynarmic)
target_link_libraries(armbat_core PRIVATE z log OpenSLES armbat_warnings)
target_compile_features(armbat_core PUBLIC cxx_std_20)
# The container is linked into one image whose only dynamic entry points are the
# JNI functions, so dead code and the symbol table can be trimmed at link time.
target_compile_options(armbat_core PRIVATE
    -ffunction-sections -fdata-sections -fvisibility=hidden)
add_dependencies(armbat_core guest_unwind guest_sysroot)

# The bionic guest runtime is opt-in until it reaches parity with the HLE one.
option(ARMBAT_GUEST_RUNTIME_BIONIC "Run the guest on the real bionic userland" OFF)
if(ARMBAT_GUEST_RUNTIME_BIONIC)
    target_compile_definitions(armbat_core PUBLIC ARMBAT_GUEST_RUNTIME_BIONIC=1)
endif()

# --- Guest toolchain ---------------------------------------------------------
# The container and the ARMv7 images it loads come from one NDK: the guest code
# is built by the same clang, driven with an explicit --target.
find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)

get_filename_component(_tool_dir "${CMAKE_C_COMPILER}" DIRECTORY)
set(_objcopy "${_tool_dir}/llvm-objcopy${CMAKE_EXECUTABLE_SUFFIX}")
set(_nm "${_tool_dir}/llvm-nm${CMAKE_EXECUTABLE_SUFFIX}")
set(_strip "${_tool_dir}/llvm-strip${CMAKE_EXECUTABLE_SUFFIX}")
if(CMAKE_OBJCOPY)
    set(_objcopy "${CMAKE_OBJCOPY}")
endif()
if(CMAKE_NM)
    set(_nm "${CMAKE_NM}")
endif()
if(CMAKE_STRIP)
    set(_strip "${CMAKE_STRIP}")
endif()

# API 21 matches the ARMv7 sysroot the guest runs on (see runtime/sysroot).
set(ARMBAT_GUEST_TRIPLE armv7a-linux-androideabi21)
set(ARMBAT_GUEST_ARCH_FLAGS
    "--target=${ARMBAT_GUEST_TRIPLE}"
    "-march=armv7-a"
    "-mfpu=neon"
    "-mfloat-abi=softfp"
)
# Linked against our own libc stub: every import stays unresolved on purpose.
set(ARMBAT_GUEST_BARE_FLAGS ${ARMBAT_GUEST_ARCH_FLAGS}
    "-O2"
    "-fPIC"
    "-shared"
    "-nostdlib"
    "-fno-exceptions"
    "-fno-asynchronous-unwind-tables"
    "-fno-unwind-tables"
    "-Wl,--build-id=none"
)
# Normally linked: keeps libc.so/libdl.so in DT_NEEDED and calls bionic instead
# of raw syscalls.
set(ARMBAT_GUEST_LINKED_FLAGS ${ARMBAT_GUEST_ARCH_FLAGS}
    "-O2"
    "-fPIC"
    "-shared"
    "-Wl,--build-id=none"
)
set(ARMBAT_GUEST_LINKED_CXX_FLAGS ${ARMBAT_GUEST_LINKED_FLAGS} "-fno-exceptions" "-fno-rtti")
set(ARMBAT_GUEST_LINKED_STD_FLAGS ${ARMBAT_GUEST_LINKED_FLAGS} "-fexceptions")

set(_guest_lib_dir "${CMAKE_BINARY_DIR}/guest")

# Embedding turns a guest image into a C array inside the container, so neither
# the APK nor the CLI has to ship guest files next to the binary.
function(armbat_embed_binary input output namespace name)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_embedded_binary.py"
                --input "${input}" --output "${output}"
                --namespace "armbat::${namespace}" --name "${name}"
        DEPENDS "${input}" "${PROJECT_SOURCE_DIR}/tools/gen_embedded_binary.py"
        COMMENT "Embedding ${name}"
        VERBATIM
    )
endfunction()

# Compile one ARMv7 module with the guest flags and strip its debug info. The
# extra arguments are additional inputs: objects or libraries to link against.
function(armbat_guest_module output soname source compiler flags)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_guest_lib_dir}"
        COMMAND "${compiler}" ${flags} "-Wl,-soname,${soname}" "${source}" ${ARGN}
                -o "${output}"
        COMMAND "${_strip}" --strip-debug "${output}"
        DEPENDS "${source}" ${ARGN}
        COMMENT "Building ${soname}"
        VERBATIM
    )
endfunction()

# --- Guest ARMv7 blob --------------------------------------------------------
set(_guest_blob "${CMAKE_BINARY_DIR}/generated/guest_blob.h")

add_custom_command(
    OUTPUT "${_guest_blob}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_guest_blob.py"
            --clang "${CMAKE_C_COMPILER}"
            --objcopy "${_objcopy}"
            --nm "${_nm}"
            --source "${PROJECT_SOURCE_DIR}/tests/guest/guest_functions.c"
            --out "${_guest_blob}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/guest/guest_functions.c"
        "${PROJECT_SOURCE_DIR}/tools/gen_guest_blob.py"
    COMMENT "Generating ARMv7 guest blob"
    VERBATIM
)

add_custom_target(guest_blob DEPENDS "${_guest_blob}")

# --- Selftest suites (guest blob and guest libraries consumers) --------------
# Cross-compiled for the device: they run over adb, not through CTest.
add_library(armbat_selftest STATIC
    src/selftest/jit_selftest.cpp
    src/selftest/loader_selftest.cpp
    "${PROJECT_SOURCE_DIR}/tests/guest/host_baseline.c"
)

target_include_directories(armbat_selftest PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
    "${PROJECT_SOURCE_DIR}/tests/guest"
)
target_link_libraries(armbat_selftest PUBLIC armbat_core PRIVATE armbat_warnings)
target_compile_features(armbat_selftest PUBLIC cxx_std_20)
add_dependencies(armbat_selftest guest_blob)

# The native baseline must differ from the guest only in ISA, not in
# auto-vectorization, otherwise the ratio measures the compiler.
set_source_files_properties(
    "${PROJECT_SOURCE_DIR}/tests/guest/host_baseline.c"
    PROPERTIES COMPILE_OPTIONS "-fno-vectorize;-fno-slp-vectorize"
)

# --- JNI surface (APK harness) -----------------------------------------------
# The suites drag their fixtures (guest libraries, test APKs, libc++) into the
# shipped image and into every native build, and nothing in the app calls
# runSelfTest(); keep them out of the APK unless asked.
option(ARMBAT_SELFTEST_IN_APK "Embed the selftest suites and fixtures in the APK" OFF)

add_library(armbat_jni SHARED src/jni/armbat_jni.cpp src/jni/armbat_container.cpp)
target_link_libraries(armbat_jni PRIVATE armbat_core log armbat_warnings)
target_compile_options(armbat_jni PRIVATE
    -ffunction-sections -fdata-sections -fvisibility=hidden)
target_link_options(armbat_jni PRIVATE -Wl,--gc-sections)
if(ARMBAT_SELFTEST_IN_APK)
    target_link_libraries(armbat_jni PRIVATE armbat_selftest)
    target_compile_definitions(armbat_jni PRIVATE ARMBAT_SELFTEST_IN_APK=1)
endif()

# An archive that only ships armeabi-v7a still calls System.loadLibrary, and the
# loader has to find a file for the name. This is that file: empty on purpose.
add_library(guestsdk SHARED src/jni/guestsdk.cpp)
target_link_libraries(guestsdk PRIVATE armbat_warnings)

# --- ARMv7 unwinder ----------------------------------------------------------
# ARM EHABI needs a real `_Unwind_*` implementation for C++ exceptions. bionic
# carries one inside libc.so; the container has to carry its own, because a
# guest image links against the platform and finds ours instead.
# The clang directory is versioned (clang/NN), so it is discovered, not assumed.
file(GLOB _ndk_unwind_candidates
    "${_tool_dir}/../lib/clang/*/lib/linux/arm/libunwind.a")
if(NOT _ndk_unwind_candidates)
    message(FATAL_ERROR
        "libunwind.a not found under ${_tool_dir}/../lib/clang: the NDK layout changed")
endif()
list(SORT _ndk_unwind_candidates)
list(GET _ndk_unwind_candidates -1 _ndk_unwind)

set(_guest_unwind_so "${_guest_lib_dir}/libarmbat_unwind.so")
set(_guest_unwind_header "${CMAKE_BINARY_DIR}/generated/guest_unwind.h")

add_custom_command(
    OUTPUT "${_guest_unwind_so}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_guest_lib_dir}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build_unwind.py"
            --clang "${CMAKE_C_COMPILER}"
            --objcopy "${_objcopy}"
            --nm "${_nm}"
            --archive "${_ndk_unwind}"
            --out "${_guest_unwind_so}"
    DEPENDS "${_ndk_unwind}" "${PROJECT_SOURCE_DIR}/tools/build_unwind.py"
    COMMENT "Building ARMv7 unwinder"
    VERBATIM
)

armbat_embed_binary("${_guest_unwind_so}" "${_guest_unwind_header}"
    guest_unwind kUnwind)

add_custom_target(guest_unwind DEPENDS "${_guest_unwind_header}")

# --- Guest resolver shim -----------------------------------------------------
# bionic resolves a hostname through netd, and the container has no netd. The
# shim exports getaddrinfo() and asks the container instead (one private trap),
# which makes a server address a player typed work the way it does on a device.
set(_guest_net_so "${_guest_lib_dir}/libarmbat_net.so")
armbat_guest_module("${_guest_net_so}" "libarmbat_net.so"
    "${PROJECT_SOURCE_DIR}/runtime/guest/net.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_LINKED_FLAGS}")

set(_guest_net_header "${CMAKE_BINARY_DIR}/generated/guest_net.h")
armbat_embed_binary("${_guest_net_so}" "${_guest_net_header}" guest_net kNet)

add_custom_target(guest_net DEPENDS "${_guest_net_header}")
add_dependencies(armbat_core guest_net)

# --- Guest bionic sysroot ----------------------------------------------------
# The guest runs the real Android userland: the linker from the ARMv7 system
# image plus the libc/libm/libdl family it loads. The files are pinned copies
# from an official SDK image (see the sysroot repository); they are embedded so
# the runtime does not depend on an asset pipeline or on the APK carrying them.
set(_sysroot_dir "${PROJECT_SOURCE_DIR}/runtime/sysroot/armeabi-v7a")

set(_sysroot_boot "${CMAKE_BINARY_DIR}/guest/armbat_boot")
set(_crt_dir "${_tool_dir}/../sysroot/usr/lib/arm-linux-androideabi/21")

add_custom_command(
    OUTPUT "${_sysroot_boot}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_guest_lib_dir}"
    COMMAND "${CMAKE_C_COMPILER}" ${ARMBAT_GUEST_ARCH_FLAGS}
            "-O2"
            "-fPIE"
            "-nostdlib"
            "-Wl,-pie"
            "-Wl,--dynamic-linker=/system/bin/linker"
            "-Wl,--build-id=none"
            "-Wl,-z,nocopyreloc"
            "${_crt_dir}/crtbegin_dynamic.o"
            "${PROJECT_SOURCE_DIR}/runtime/guest/boot.c"
            "${_sysroot_dir}/lib/libc.so"
            "${_sysroot_dir}/lib/libdl.so"
            "${_crt_dir}/crtend_android.o"
            "-o" "${_sysroot_boot}"
    DEPENDS
        "${PROJECT_SOURCE_DIR}/runtime/guest/boot.c"
        "${_crt_dir}/crtbegin_dynamic.o"
    COMMENT "Building the guest bionic boot program"
    VERBATIM
)

# One embedded image per file, named after the guest path it serves.
set(_sysroot_embeds
    "linker:bin/linker:sysroot_linker:kLinker"
    "libc:lib/libc.so:sysroot_libc:kLibc"
    "libm:lib/libm.so:sysroot_libm:kLibm"
    "libdl:lib/libdl.so:sysroot_libdl:kLibdl"
    "libstdcxx:lib/libstdc++.so:sysroot_libstdcpp:kLibstdcpp"
    "libz:lib/libz.so:sysroot_libz:kLibz"
)

set(_sysroot_header_list "")
foreach(_entry IN LISTS _sysroot_embeds)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _key)
    list(GET _parts 1 _rel)
    list(GET _parts 2 _ns)
    list(GET _parts 3 _name)
    set(_header "${CMAKE_BINARY_DIR}/generated/${_ns}.h")
    armbat_embed_binary("${_sysroot_dir}/${_rel}" "${_header}" "${_ns}" "${_name}")
    list(APPEND _sysroot_header_list "${_header}")
endforeach()

set(_boot_header "${CMAKE_BINARY_DIR}/generated/guest_boot.h")
armbat_embed_binary("${_sysroot_boot}" "${_boot_header}" guest_boot kBoot)

add_custom_target(guest_sysroot DEPENDS ${_sysroot_header_list} "${_boot_header}")

# --- Guest platform shims ----------------------------------------------------
# A game links against libEGL, libGLESv2, libandroid, liblog, libOpenSLES and
# friends. The device's implementations cannot run here (they talk to real
# drivers), so the container answers those APIs itself. The guest still has to
# load a library with that name and resolve every imported symbol, so each shim
# exports the platform's own API surface - taken from the NDK's API-24 stub
# library - as trampolines that trap into the HLE registry by symbol name.
set(_shim_work "${CMAKE_BINARY_DIR}/shims")
set(_shim_libraries
    libEGL libGLESv1_CM libGLESv2 libandroid libjnigraphics liblog libOpenSLES)
set(_shim_outputs "")
# The generator reads the NDK stubs; a re-installed NDK has to rebuild the shims.
set(_shim_stub_deps "")
foreach(_library IN LISTS _shim_libraries)
    list(APPEND _shim_outputs "${_shim_work}/lib/${_library}.so")
    file(GLOB _stubs
        "${ANDROID_NDK}/toolchains/llvm/prebuilt/*/sysroot/usr/lib/arm-linux-androideabi/24/${_library}.so")
    list(APPEND _shim_stub_deps ${_stubs})
endforeach()
add_custom_command(
    OUTPUT ${_shim_outputs}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_shim_work}/lib"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/gen_platform_shims.py"
            --ndk "${ANDROID_NDK}" --out "${_shim_work}" ${_shim_libraries}
    DEPENDS "${PROJECT_SOURCE_DIR}/tools/gen_platform_shims.py" ${_shim_stub_deps}
    COMMENT "Building the guest platform shim libraries"
    VERBATIM
)
add_custom_target(guest_shims DEPENDS ${_shim_outputs})

# One embedded image per shim, so the runtime carries them inside the JNI
# library and writes them into the guest sysroot on first launch.
set(_shim_embeds
    "libEGL:libegl:kEgl"
    "libGLESv1_CM:libglesv1:kGlesv1"
    "libGLESv2:libglesv2:kGlesv2"
    "libandroid:libandroid:kAndroid"
    "libjnigraphics:libjnigraphics:kJnigraphics"
    "liblog:liblog:kLog"
    "libOpenSLES:libopensles:kOpensles"
)
set(_shim_header_list "")
foreach(_entry IN LISTS _shim_embeds)
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _library)
    list(GET _parts 1 _stem)
    list(GET _parts 2 _name)
    set(_header "${CMAKE_BINARY_DIR}/generated/shim_${_stem}.h")
    armbat_embed_binary("${_shim_work}/lib/${_library}.so" "${_header}" shims "${_name}")
    list(APPEND _shim_header_list "${_header}")
endforeach()
add_custom_target(guest_shim_headers DEPENDS ${_shim_header_list})
add_dependencies(armbat_core guest_shim_headers)

# --- Guest worker stub -------------------------------------------------------
# Platform threads (ART) cannot hold an ARM frame: their registers, stack and
# TLS are the host's, and two of them can be in guest code at once. This stub
# gives the container real guest threads to run such a call on -- see
# runtime/guest/attach.c and GuestContainer::RunOnGuestWorker.
set(_guest_attach_so "${_guest_lib_dir}/libarmbat_attach.so")
set(_guest_attach_header "${CMAKE_BINARY_DIR}/generated/guest_attach.h")

armbat_guest_module("${_guest_attach_so}" libarmbat_attach.so
    "${PROJECT_SOURCE_DIR}/runtime/guest/attach.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_LINKED_FLAGS}")
armbat_embed_binary("${_guest_attach_so}" "${_guest_attach_header}"
    guest_attach kAttach)

add_custom_target(guest_attach DEPENDS "${_guest_attach_header}")
add_dependencies(armbat_core guest_attach)

# --- ELF32 loader fixtures ---------------------------------------------------
# Consumed by the loader selftest only; the APK build never reaches them.
set(_guest_lib_so "${_guest_lib_dir}/libguesttest.so")
set(_guest_lib_missing_so "${_guest_lib_dir}/libguesttest_missing.so")
set(_guest_lib_imports_so "${_guest_lib_dir}/libguesttest_imports.so")
set(_guest_lib_syscalls_so "${_guest_lib_dir}/libguesttest_syscalls.so")
set(_guest_stub_so "${_guest_lib_dir}/libc.so")
set(_guest_lib_needed_so "${_guest_lib_dir}/libguesttest_needed.so")
set(_guest_lib_real_so "${_guest_lib_dir}/libguesttest_real.so")
set(_guest_lib_cpp_so "${_guest_lib_dir}/libguesttest_cpp.so")
set(_guest_lib_stdio_so "${_guest_lib_dir}/libguesttest_stdio.so")
set(_guest_lib_pthread_so "${_guest_lib_dir}/libguesttest_pthread.so")
set(_guest_lib_cppstdlib_so "${_guest_lib_dir}/libguesttest_cppstdlib.so")
set(_guest_lib_dep_so "${_guest_lib_dir}/libguesttest_dep.so")
set(_guest_libcxx_so "${_guest_lib_dir}/libc++_shared.so")
set(_guest_test_apk "${_guest_lib_dir}/armbat-test.apk")
set(_guest_test_apk_armeabi "${_guest_lib_dir}/armbat-test-armeabi.apk")
set(_guest_lib_header "${CMAKE_BINARY_DIR}/generated/guest_lib.h")
set(_guest_lib_missing_header "${CMAKE_BINARY_DIR}/generated/guest_lib_missing.h")
set(_guest_lib_imports_header "${CMAKE_BINARY_DIR}/generated/guest_lib_imports.h")
set(_guest_lib_syscalls_header "${CMAKE_BINARY_DIR}/generated/guest_lib_syscalls.h")
set(_guest_lib_needed_header "${CMAKE_BINARY_DIR}/generated/guest_lib_needed.h")
set(_guest_lib_real_header "${CMAKE_BINARY_DIR}/generated/guest_lib_real.h")
set(_guest_lib_cpp_header "${CMAKE_BINARY_DIR}/generated/guest_lib_cpp.h")
set(_guest_lib_stdio_header "${CMAKE_BINARY_DIR}/generated/guest_lib_stdio.h")
set(_guest_lib_pthread_header "${CMAKE_BINARY_DIR}/generated/guest_lib_pthread.h")
set(_guest_lib_cppstdlib_header "${CMAKE_BINARY_DIR}/generated/guest_lib_cppstdlib.h")
set(_guest_libcxx_header "${CMAKE_BINARY_DIR}/generated/guest_libcxx.h")
set(_guest_test_apk_header "${CMAKE_BINARY_DIR}/generated/guest_apk.h")
set(_guest_test_apk_armeabi_header "${CMAKE_BINARY_DIR}/generated/guest_apk_armeabi.h")

# The arm32 libc++ that ships with the NDK. It is the same file the app's asset
# folder carries, so the CLI and the APK run against identical input.
set(_ndk_libcxx
    "${_tool_dir}/../sysroot/usr/lib/arm-linux-androideabi/libc++_shared.so")

# One library per loader/ELF case: unresolved import, libc imports, raw
# syscalls, DT_NEEDED, C++ with and without the standard library, stdio,
# pthreads, and a library that depends on another one from the same APK.
armbat_guest_module("${_guest_lib_so}" libguesttest.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}")
armbat_embed_binary("${_guest_lib_so}" "${_guest_lib_header}" guest_lib kGuestLib)

armbat_guest_module("${_guest_lib_missing_so}" libguesttest_missing.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_missing.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}")
armbat_embed_binary("${_guest_lib_missing_so}" "${_guest_lib_missing_header}"
    guest_lib_missing kMissingLib)

armbat_guest_module("${_guest_lib_imports_so}" libguesttest_imports.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_imports.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}")
armbat_embed_binary("${_guest_lib_imports_so}" "${_guest_lib_imports_header}"
    guest_lib_imports kImportsLib)

armbat_guest_module("${_guest_lib_syscalls_so}" libguesttest_syscalls.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_syscalls.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}")
armbat_embed_binary("${_guest_lib_syscalls_so}" "${_guest_lib_syscalls_header}"
    guest_lib_syscalls kSyscallsLib)

armbat_guest_module("${_guest_stub_so}" libc.so
    "${PROJECT_SOURCE_DIR}/tests/guest/libc_stub.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}")

armbat_guest_module("${_guest_lib_needed_so}" libguesttest_needed.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_needed.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}" "${_guest_stub_so}")
armbat_embed_binary("${_guest_lib_needed_so}" "${_guest_lib_needed_header}"
    guest_lib_needed kNeededLib)

armbat_guest_module("${_guest_lib_real_so}" libguesttest_real.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_real.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_LINKED_FLAGS}")
armbat_embed_binary("${_guest_lib_real_so}" "${_guest_lib_real_header}"
    guest_lib_real kRealLib)

armbat_guest_module("${_guest_lib_cpp_so}" libguesttest_cpp.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_cpp.cpp"
    "${CMAKE_CXX_COMPILER}" "${ARMBAT_GUEST_LINKED_CXX_FLAGS}")
armbat_embed_binary("${_guest_lib_cpp_so}" "${_guest_lib_cpp_header}"
    guest_lib_cpp kCppLib)

armbat_guest_module("${_guest_lib_stdio_so}" libguesttest_stdio.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_stdio.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_LINKED_FLAGS}")
armbat_embed_binary("${_guest_lib_stdio_so}" "${_guest_lib_stdio_header}"
    guest_lib_stdio kStdioLib)

armbat_guest_module("${_guest_lib_pthread_so}" libguesttest_pthread.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_pthread.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_LINKED_FLAGS}")
armbat_embed_binary("${_guest_lib_pthread_so}" "${_guest_lib_pthread_header}"
    guest_lib_pthread kPthreadLib)

armbat_guest_module("${_guest_lib_cppstdlib_so}" libguesttest_cppstdlib.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_cppstdlib.cpp"
    "${CMAKE_CXX_COMPILER}" "${ARMBAT_GUEST_LINKED_STD_FLAGS}")
armbat_embed_binary("${_guest_lib_cppstdlib_so}" "${_guest_lib_cppstdlib_header}"
    guest_lib_cppstdlib kCppStdLib)

# The real arm32 libc++ from the NDK. Debug info is stripped so the embedded copy
# stays around 1.2 MiB; the app will ship the same file as an asset.
add_custom_command(
    OUTPUT "${_guest_libcxx_so}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_guest_lib_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_ndk_libcxx}" "${_guest_libcxx_so}"
    COMMAND "${_strip}" --strip-debug "${_guest_libcxx_so}"
    DEPENDS "${_ndk_libcxx}"
    COMMENT "Staging the ARMv7 libc++"
    VERBATIM
)
armbat_embed_binary("${_guest_libcxx_so}" "${_guest_libcxx_header}"
    guest_libcxx kLibcxx)

# An application library that depends on another library from the same APK.
armbat_guest_module("${_guest_lib_dep_so}" libguesttest_dep.so
    "${PROJECT_SOURCE_DIR}/tests/guest/native_lib_dep.c"
    "${CMAKE_C_COMPILER}" "${ARMBAT_GUEST_BARE_FLAGS}" "${_guest_lib_so}")

add_custom_command(
    OUTPUT "${_guest_test_apk}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/make_test_apk.py"
            --library "${_guest_lib_so}" --extra-library "${_guest_lib_dep_so}"
            --output "${_guest_test_apk}"
    DEPENDS "${_guest_lib_so}" "${_guest_lib_dep_so}"
            "${PROJECT_SOURCE_DIR}/tools/make_test_apk.py"
    COMMENT "Building the APK fixture"
    VERBATIM
)
armbat_embed_binary("${_guest_test_apk}" "${_guest_test_apk_header}"
    guest_apk_fixture kTestApk)

# The same archive as Android packaged it before armeabi-v7a existed: the
# libraries live in lib/armeabi, and the container has to find them there.
add_custom_command(
    OUTPUT "${_guest_test_apk_armeabi}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/make_test_apk.py"
            --library "${_guest_lib_so}" --extra-library "${_guest_lib_dep_so}"
            --abi armeabi --output "${_guest_test_apk_armeabi}"
    DEPENDS "${_guest_lib_so}" "${_guest_lib_dep_so}"
            "${PROJECT_SOURCE_DIR}/tools/make_test_apk.py"
    COMMENT "Building the legacy-ABI APK fixture"
    VERBATIM
)
armbat_embed_binary("${_guest_test_apk_armeabi}" "${_guest_test_apk_armeabi_header}"
    guest_apk_armeabi kTestApk)

add_custom_target(guest_libs DEPENDS
    "${_guest_lib_header}"
    "${_guest_lib_missing_header}"
    "${_guest_lib_imports_header}"
    "${_guest_lib_syscalls_header}"
    "${_guest_lib_needed_header}"
    "${_guest_lib_real_header}"
    "${_guest_lib_cpp_header}"
    "${_guest_lib_stdio_header}"
    "${_guest_lib_pthread_header}"
    "${_guest_lib_cppstdlib_header}"
    "${_guest_libcxx_header}"
    "${_guest_test_apk_header}"
    "${_guest_test_apk_armeabi_header}")
add_dependencies(armbat_selftest guest_libs)
