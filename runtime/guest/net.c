// The guest's own libc cannot answer a name lookup on this platform: bionic
// forwards getaddrinfo() to netd over a socket, and a container has no netd.
// This library is mapped into the global symbol group before the application,
// so the application's lookups land here, and the answer comes from the host
// process -- whose resolver is the device's own.
//
// Nothing else is replaced: the trap carries the question out and the answer
// back, and addrinfo is built here with the guest's allocator so that the
// application frees what it was given.

#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

// Armbat's private syscall range (see guest_kernel.cpp).
#define ARMBAT_SVC_RESOLVE (0x0f0000u + 0x101u)

#if defined(__arm__)
static long armbat_resolve(const char* name, void* buffer, unsigned long capacity) {
    register long r0 __asm__("r0") = (long)name;
    register long r1 __asm__("r1") = (long)buffer;
    register long r2 __asm__("r2") = (long)capacity;
    register long r7 __asm__("r7") = (long)ARMBAT_SVC_RESOLVE;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}
#else
static long armbat_resolve(const char* name, void* buffer, unsigned long capacity) {
    (void)name;
    (void)buffer;
    (void)capacity;
    return -1;
}
#endif

// The packed answer: a count, then one entry per address -- family, address
// length, and the address bytes padded to a word. It is a wire format, not an
// ABI the guest ever sees as a structure.
#define ARMBAT_RESULT_ENTRIES 8

static unsigned int armbat_padded(unsigned int length) {
    return (length + 3u) & ~3u;
}

void freeaddrinfo(struct addrinfo* list);

static unsigned short armbat_port(const char* service, int* ok) {
    *ok = 1;
    if (service == NULL) {
        return 0;
    }
    char* end = NULL;
    const long value = strtol(service, &end, 10);
    if (end == service || end == NULL || *end != '\0' || value < 0 || value > 65535) {
        *ok = 0;
        return 0;
    }
    return htons((unsigned short)value);
}

static struct addrinfo* armbat_entry(unsigned int family, const void* bytes,
                                     const struct addrinfo* settings, unsigned short port) {
    struct addrinfo* entry = (struct addrinfo*)calloc(1, sizeof(struct addrinfo));
    if (entry == NULL) {
        return NULL;
    }
    if (family == AF_INET) {
        struct sockaddr_in* address = (struct sockaddr_in*)calloc(1, sizeof(struct sockaddr_in));
        if (address == NULL) {
            free(entry);
            return NULL;
        }
        address->sin_family = AF_INET;
        address->sin_port = port;
        memcpy(&address->sin_addr, bytes, 4);
        entry->ai_addr = (struct sockaddr*)address;
        entry->ai_addrlen = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6* address = (struct sockaddr_in6*)calloc(1, sizeof(struct sockaddr_in6));
        if (address == NULL) {
            free(entry);
            return NULL;
        }
        address->sin6_family = AF_INET6;
        address->sin6_port = port;
        memcpy(&address->sin6_addr, bytes, 16);
        entry->ai_addr = (struct sockaddr*)address;
        entry->ai_addrlen = sizeof(struct sockaddr_in6);
    }
    entry->ai_family = (int)family;
    entry->ai_socktype = settings->ai_socktype;
    entry->ai_protocol = settings->ai_protocol;
    return entry;
}

static int armbat_getaddrinfo(const char* node, const char* service,
                              const struct addrinfo* hints, struct addrinfo** result) {
    if (result == NULL) {
        return EAI_FAIL;
    }
    *result = NULL;

    struct addrinfo settings;
    memset(&settings, 0, sizeof(settings));
    if (hints != NULL) {
        settings = *hints;
    }
    if (settings.ai_family != AF_INET && settings.ai_family != AF_INET6) {
        settings.ai_family = AF_UNSPEC;
    }
    if (settings.ai_socktype == 0) {
        settings.ai_socktype = SOCK_STREAM;
    }

    int port_ok = 1;
    const unsigned short port = armbat_port(service, &port_ok);
    if (!port_ok) {
        return EAI_SERVICE;
    }

    // A server asks for the local wildcard address, not for a name: there is
    // nothing to resolve, and every port-based server in a LAN game starts here.
    if (node == NULL) {
        static const unsigned char any4[4] = {0, 0, 0, 0};
        static const unsigned char any6[16] = {0};
        const unsigned int families[2] = {AF_INET, AF_INET6};
        struct addrinfo* head = NULL;
        struct addrinfo** tail = &head;
        for (int index = 0; index < 2; ++index) {
            const unsigned int family = families[index];
            if (settings.ai_family != AF_UNSPEC && settings.ai_family != (int)family) {
                continue;
            }
            struct addrinfo* entry =
                armbat_entry(family, family == AF_INET ? (const void*)any4 : (const void*)any6,
                             &settings, port);
            if (entry == NULL) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            *tail = entry;
            tail = &entry->ai_next;
        }
        if (head == NULL) {
            return EAI_NONAME;
        }
        *result = head;
        return 0;
    }

    unsigned char packed[4 + ARMBAT_RESULT_ENTRIES * (8 + 16)];
    const long length = armbat_resolve(node, packed, sizeof(packed));
    if (length < 4) {
        return EAI_NONAME;
    }
    unsigned int count = 0;
    memcpy(&count, packed, 4);

    struct addrinfo* head = NULL;
    struct addrinfo** tail = &head;
    unsigned long offset = 4;
    for (unsigned int index = 0; index < count; ++index) {
        unsigned int family = 0;
        unsigned int address_length = 0;
        if (offset + 8 > (unsigned long)length) {
            break;
        }
        memcpy(&family, packed + offset, 4);
        memcpy(&address_length, packed + offset + 4, 4);
        offset += 8;
        if (address_length > 16 || offset + address_length > (unsigned long)length) {
            break;
        }
        const int wanted = (family == AF_INET || family == AF_INET6) &&
                           (settings.ai_family == AF_UNSPEC || settings.ai_family == (int)family) &&
                           (family == AF_INET ? address_length == 4 : address_length == 16);
        if (wanted) {
            struct addrinfo* entry = armbat_entry(family, packed + offset, &settings, port);
            if (entry == NULL) {
                freeaddrinfo(head);
                return EAI_MEMORY;
            }
            *tail = entry;
            tail = &entry->ai_next;
        }
        offset += armbat_padded(address_length);
    }
    if (head == NULL) {
        return EAI_NONAME;
    }
    *result = head;
    return 0;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints,
                struct addrinfo** result) {
    return armbat_getaddrinfo(node, service, hints, result);
}

int android_getaddrinfofornet(const char* node, const char* service,
                              const struct addrinfo* hints, unsigned netid, unsigned mark,
                              struct addrinfo** result) {
    (void)netid;
    (void)mark;
    return armbat_getaddrinfo(node, service, hints, result);
}

int android_getaddrinfofornetcontext(const char* node, const char* service,
                                     const struct addrinfo* hints, const void* context,
                                     struct addrinfo** result) {
    (void)context;
    return armbat_getaddrinfo(node, service, hints, result);
}

void freeaddrinfo(struct addrinfo* list) {
    while (list != NULL) {
        struct addrinfo* next = list->ai_next;
        free(list->ai_addr);
        free(list->ai_canonname);
        free(list);
        list = next;
    }
}

const char* gai_strerror(int error) {
    switch (error) {
    case 0:
        return "no error";
    case EAI_AGAIN:
        return "temporary failure in name resolution";
    case EAI_BADFLAGS:
        return "invalid flags";
    case EAI_FAIL:
        return "non-recoverable failure in name resolution";
    case EAI_FAMILY:
        return "address family not supported";
    case EAI_MEMORY:
        return "out of memory";
    case EAI_NONAME:
        return "name or service not known";
    case EAI_SERVICE:
        return "service not supported";
    case EAI_SOCKTYPE:
        return "socket type not supported";
    case EAI_SYSTEM:
        return "system error";
    default:
        return "unknown error";
    }
}

// The IPv4-only entry point an older engine still calls. The answer is kept in
// statics because that is the interface bionic exposes.
struct hostent* gethostbyname(const char* name) {
    static struct hostent entry;
    static struct in_addr address;
    static char* aliases[1];
    static char* addresses[2];
    static char name_buffer[256];

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* result = NULL;
    if (armbat_getaddrinfo(name, NULL, &hints, &result) != 0 || result == NULL) {
        return NULL;
    }
    const struct sockaddr_in* found = (const struct sockaddr_in*)result->ai_addr;
    address = found->sin_addr;
    freeaddrinfo(result);

    const unsigned long length = strlen(name);
    if (length >= sizeof(name_buffer)) {
        return NULL;
    }
    memcpy(name_buffer, name, length + 1);
    aliases[0] = NULL;
    addresses[0] = (char*)&address;
    addresses[1] = NULL;
    entry.h_name = name_buffer;
    entry.h_aliases = aliases;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = addresses;
    return &entry;
}
