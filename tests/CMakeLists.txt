# The binaries below are cross-compiled for the device and run over adb (see
# README), so they are deliberately not registered with ctest: a host-side
# `ctest` could only ever report "exec format error".
add_executable(armbat-selftest runtime/selftest_cli.cpp)
target_link_libraries(armbat-selftest PRIVATE armbat_selftest armbat_warnings)
target_compile_features(armbat-selftest PRIVATE cxx_std_20)

# Headless container: `armbat-apk <application.apk>` maps the APK's ARMv7
# libraries and prints what happened. Same class the shell uses.
add_executable(armbat-apk apk_tool.cpp)
target_link_libraries(armbat-apk PRIVATE armbat_core armbat_warnings)
target_compile_features(armbat-apk PRIVATE cxx_std_20)
