#include "armbat/guest_properties.h"

#include <cstddef>
#include <cstring>

#include <sys/system_properties.h>

namespace armbat {
namespace {

struct GuestPropertyEntry {
    const char* name;
    const char* value;
};

// The platform fields of the device the guest is told it runs on: an ARMv7
// system whose bionic is the API-24 sysroot. The guest's own bionic reads its
// property area from /dev/__properties__, which the container does not map, so
// without this table every __system_property_get() would answer an empty value
// -- a state no Android device produces, and these fields are what an
// application reads to decide which system it runs on. The host's answers
// cannot be passed through either: the guest is a 32-bit process, and this
// device has no 32-bit ABI to name.
//
// The set is one coherent device, not a list of comfortable answers: an
// application that cross-checks them -- the SDK level against the fingerprint,
// the ABI against the model, the build tags against the boot state -- has to
// find a system that could exist. Mixing the guest's ABI into a fingerprint
// taken from this phone produces a device that never shipped, which is what a
// repackaged application looks like to the integrity checks these engines run
// besides the game.
constexpr GuestPropertyEntry kGuestProperties[] = {
    {"ro.build.version.sdk", "24"},
    {"ro.build.version.release", "7.0"},
    {"ro.build.version.codename", "REL"},
    {"ro.build.version.all_codenames", "REL"},
    {"ro.build.version.incremental", "J500FXXU1BQH1"},
    {"ro.build.version.security_patch", "2016-10-01"},
    {"ro.build.id", "NRD90M"},
    {"ro.build.display.id", "NRD90M.J500FXXU1BQH1"},
    {"ro.build.date", "Wed Sep 20 10:13:53 KST 2017"},
    {"ro.build.date.utc", "1505870033"},
    {"ro.build.type", "user"},
    {"ro.build.tags", "release-keys"},
    {"ro.build.user", "dpi"},
    {"ro.build.host", "SWDD6514"},
    {"ro.build.flavor", "j5ltexx-user"},
    {"ro.build.product", "j5lte"},
    {"ro.build.characteristics", "default"},
    {"ro.build.description", "j5ltexx-user 7.0 NRD90M J500FXXU1BQH1 release-keys"},
    {"ro.build.fingerprint",
     "samsung/j5ltexx/j5lte:7.0/NRD90M/J500FXXU1BQH1:user/release-keys"},
    {"ro.debuggable", "0"},
    {"ro.secure", "1"},
    {"ro.adb.secure", "1"},
    {"ro.allow.mock.location", "0"},
    {"ro.kernel.qemu", "0"},
    {"ro.boot.qemu", "0"},
    {"ro.hardware", "qcom"},
    {"ro.board.platform", "msm8916"},
    {"ro.chipname", "msm8916"},
    {"ro.product.brand", "samsung"},
    {"ro.product.manufacturer", "samsung"},
    {"ro.product.model", "SM-J500F"},
    {"ro.product.device", "j5lte"},
    {"ro.product.name", "j5ltexx"},
    {"ro.product.board", "msm8916"},
    {"ro.product.locale", "en-GB"},
    {"ro.product.locale.language", "en"},
    {"ro.product.locale.region", "GB"},
    {"ro.product.cpu.abi", "armeabi-v7a"},
    {"ro.product.cpu.abi2", "armeabi"},
    {"ro.product.cpu.abilist", "armeabi-v7a,armeabi"},
    {"ro.product.cpu.abilist32", "armeabi-v7a,armeabi"},
    {"ro.product.cpu.abilist64", ""},
    {"ro.product.first_api_level", "22"},
    {"ro.sf.lcd_density", "320"},
    {"ro.opengles.version", "196609"},
    {"ro.zygote", "zygote32"},
    {"ro.dalvik.vm.native.bridge", "0"},
    {"ro.serialno", "R58M12345AB"},
    {"ro.boot.serialno", "R58M12345AB"},
    {"ro.boot.hardware", "qcom"},
    {"ro.boot.baseband", "msm"},
    {"ro.boot.bootloader", "J500FXXU1BQH1"},
    {"ro.boot.verifiedbootstate", "green"},
    {"persist.sys.dalvik.vm.lib.2", "libart.so"},
    {"persist.sys.timezone", "Europe/London"},
    {"net.hostname", "android-4a1c9f02b7e5"},
    {"sys.boot_completed", "1"},
    {"dev.bootcomplete", "1"},
    {"sys.usb.state", "mtp"},
};

// Whether the name belongs to the namespaces that identify the device. These
// are the ones where this phone's own answer contradicts the profile above --
// it is arm64 and API 36 while the guest is neither -- and a contradiction is
// the one thing an application cannot be handed. The runtime switches a guest
// legitimately reads through the same interface (sys.*, net.*, debug.*) are not
// in this list: their host answer is the right one.
bool IdentityNamespace(const std::string& name) {
    static constexpr const char* kNamespaces[] = {"ro.",  "persist.", "gsm.",
                                                   "dalvik.", "init.", "vendor."};
    for (const char* prefix : kNamespaces) {
        const std::size_t length = std::strlen(prefix);
        if (name.size() >= length && name.compare(0, length, prefix) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool ResolveGuestProperty(const std::string& name, std::string* value) {
    value->clear();
    for (const GuestPropertyEntry& entry : kGuestProperties) {
        if (name == entry.name) {
            value->assign(entry.value);
            return true;
        }
    }
    if (IdentityNamespace(name)) {
        return false;
    }
    char host[PROP_VALUE_MAX + 1] = {};
    const int length = ::__system_property_get(name.c_str(), host);
    if (length > 0) {
        value->assign(host, static_cast<std::size_t>(length));
        return true;
    }
    return false;
}

}  // namespace armbat
