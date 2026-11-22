/*
 * The platform properties the guest reads.
 *
 * bionic keeps them in an area it maps from /dev/__properties__, and this
 * container has no such area: the guest's own __system_property_get() would
 * answer 0 for every name -- a device that does not exist, and the one thing an
 * application cannot be handed. This library is mapped into the global symbol
 * group before the application, so the names the guest asks for land here and
 * the answer comes from the container: the guest's own device profile for
 * anything that identifies the device, the host's real value for the switches
 * that describe the platform rather than the device.
 *
 * The same answer is served through find()/read()/serial(), with the prop_info
 * layout the guest's own bionic expects from a legacy area (name, serial,
 * value). read() and serial() are here too, so a bionic that parses its own
 * layout never sees the difference.
 */

#include <stdint.h>

#define ARMBAT_SVC_PROPERTY (0x0f0000u + 0x102u)

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92
/* Enough for a caller that holds a few results while it compares them. */
#define PROP_SLOTS 16

struct prop_info {
    char name[PROP_NAME_MAX];
    unsigned serial;
    char value[PROP_VALUE_MAX];
};

/* bionic encodes "how long is the value" in the top byte of the serial and
 * keeps the low bit for the write-in-progress flag. Nothing writes here, so the
 * value is always clean. */
#define PROP_SERIAL(length, counter) (((unsigned)(length) << 24) | (unsigned)(counter))

static struct prop_info prop_slots[PROP_SLOTS];
static unsigned prop_next_slot;

static long armbat_property(const char* name, char* value) {
    register long r0 __asm__("r0") = (long)name;
    register long r1 __asm__("r1") = (long)value;
    register long r7 __asm__("r7") = (long)ARMBAT_SVC_PROPERTY;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory");
    return r0;
}

static int name_fits(const char* name) {
    unsigned length = 0;
    while (name[length] != '\0') {
        if (++length > PROP_NAME_MAX) {
            return 0;
        }
    }
    return length > 0;
}

int __system_property_get(const char* name, char* value) {
    if (value == 0) {
        return 0;
    }
    value[0] = '\0';
    if (name == 0 || !name_fits(name)) {
        return 0;
    }
    const long length = armbat_property(name, value);
    return length > 0 ? (int)length : 0;
}

const struct prop_info* __system_property_find(const char* name) {
    if (name == 0 || !name_fits(name)) {
        return 0;
    }
    struct prop_info* slot = &prop_slots[prop_next_slot];
    prop_next_slot = (prop_next_slot + 1) % PROP_SLOTS;
    const long length = armbat_property(name, slot->value);
    if (length < 0) {
        return 0;
    }
    unsigned index = 0;
    while (name[index] != '\0' && index + 1 < PROP_NAME_MAX) {
        slot->name[index] = name[index];
        ++index;
    }
    slot->name[index] = '\0';
    slot->serial = PROP_SERIAL(length, 2 * prop_next_slot + 2);
    return slot;
}

int __system_property_read(const struct prop_info* info, char* name, char* value) {
    if (info == 0) {
        return 0;
    }
    const unsigned length = info->serial >> 24;
    if (value != 0) {
        unsigned index = 0;
        while (index < length && index + 1 < PROP_VALUE_MAX) {
            value[index] = info->value[index];
            ++index;
        }
        value[index] = '\0';
    }
    if (name != 0) {
        unsigned index = 0;
        while (index + 1 < PROP_NAME_MAX && info->name[index] != '\0') {
            name[index] = info->name[index];
            ++index;
        }
        name[index] = '\0';
    }
    return (int)length;
}

unsigned __system_property_serial(const struct prop_info* info) {
    return info == 0 ? 0u : info->serial;
}
