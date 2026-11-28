#pragma once

#include <cstdint>

// Instrumenting the container on the profiler's timeline.
//
// A zone carries a name, a thread, a nesting depth and a duration; an
// instrumented mutex reports wait and hold time per named lock. That is the
// difference between "this thread sat somewhere for 23 ms" and "this thread
// waited 23 ms for the asset table, which another thread held while it inflated
// a pack". A capture needs no permission from the platform and no second APK:
// the client is a socket inside the process.
//
// TRACY_ON_DEMAND is set in the build, so nothing is recorded until a profiler
// connects: a normal run pays a compare per zone. In a release build
// (ARMBAT_TRACY unset) every macro below is the identity, which is why the
// zones and lock wrappers can stay in the code instead of being added for one
// hunt and removed after.

#if defined(ARMBAT_TRACY)

#include <tracy/Tracy.hpp>

// The zone name is a compile-time constant: Tracy keys the source location on
// it. For a name known only at run time, open a zone with a literal and then
// rename it -- the profiler copies the string when the name is emitted.
#define ARMBAT_ZONE(name) ZoneScopedN(name)
#define ARMBAT_ZONE_IF(name, active) ZoneNamedN(armbat_zone, name, active)
#define ARMBAT_ZONE_RENAME(text, size) ZoneName(text, size)
#define ARMBAT_ZONE_RENAME_IF(variable, text, size) ZoneNameV(variable, text, size)
#define ARMBAT_THREAD_NAME(name) ::tracy::SetThreadName(name)
#define ARMBAT_FRAME() MarkFrame()
#define ARMBAT_PLOT(name, value) TracyPlot(name, static_cast<std::int64_t>(value))
#define ARMBAT_LOCKABLE(type, variable) TracyLockable(type, variable)

#else

#define ARMBAT_ZONE(name) ((void)0)
#define ARMBAT_ZONE_IF(name, active) ((void)0)
#define ARMBAT_ZONE_RENAME(text, size) ((void)0)
#define ARMBAT_ZONE_RENAME_IF(variable, text, size) ((void)0)
#define ARMBAT_THREAD_NAME(name) ((void)0)
#define ARMBAT_FRAME() ((void)0)
#define ARMBAT_PLOT(name, value) ((void)0)
#define ARMBAT_LOCKABLE(type, variable) type variable

#endif
