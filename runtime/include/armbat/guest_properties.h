#pragma once

#include <string>

namespace armbat {

// The device a guest is told it runs on.
//
// bionic reads properties from an area the container does not have, so without
// help every name would answer an empty string -- a state no device is in, and
// the one an application cannot be handed. The container has one coherent
// device to describe instead: a 32-bit API-24 phone (see guest_properties.cpp).
//
// The answer is the profile first; a name the profile does not carry falls back
// to the host for the namespaces that describe how the platform is switched
// (sys.*, net.*, debug.*), and stays empty for the ones that describe which
// device this is. Mixing this phone's identity into an armv7 guest produces a
// device that never shipped, which is exactly what a repackaged application
// looks like to an integrity check.
bool ResolveGuestProperty(const std::string& name, std::string* value);

}  // namespace armbat
