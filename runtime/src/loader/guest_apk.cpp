#include "armbat/guest_apk.h"

#include <zlib.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "armbat/dynamic_loader.h"
#include "armbat/elf32_loader.h"

namespace armbat {
namespace {

// "/a/b" and "./a/b" mean "a/b" to the asset manager; it never leaves assets/.
std::string AssetRelative(const std::string& name) {
    std::string relative = name;
    const std::size_t first = relative.find_first_not_of('/');
    relative = first == std::string::npos ? std::string() : relative.substr(first);
    if (relative.rfind("./", 0) == 0) {
        relative.erase(0, 2);
    }
    return relative;
}

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::size_t kEndOfCentralDirectorySize = 22;
constexpr std::size_t kMaxComment = 0xFFFF;

std::uint16_t Read16(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t Read32(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool EndsWith(const std::string& text, const char* suffix) {
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

}  // namespace

GuestApk::~GuestApk() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool GuestApk::ReadAt(std::uint64_t offset, void* out, std::size_t length) const {
    auto* bytes = static_cast<std::uint8_t*>(out);
    while (length != 0) {
        const ssize_t got = ::pread(fd_, bytes, length, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (got == 0) {
            return false;
        }
        bytes += got;
        offset += static_cast<std::uint64_t>(got);
        length -= static_cast<std::size_t>(got);
    }
    return true;
}

bool GuestApk::ExtractEntry(const Entry& entry, const std::string& name,
                            std::vector<std::uint8_t>* out, std::string* error) const {
    std::uint8_t header[30];
    if (!ReadAt(entry.local_offset, header, sizeof(header))) {
        if (error) *error = "cannot read the local header of '" + name + "'";
        return false;
    }
    if (Read32(header, 0) != kLocalHeader) {
        if (error) *error = "local header for '" + name + "' is corrupt";
        return false;
    }
    const std::uint16_t name_length = Read16(header, 26);
    const std::uint16_t extra_length = Read16(header, 28);
    const std::uint64_t data_offset =
        entry.local_offset + 30 + name_length + extra_length;
    if (data_offset + entry.compressed_size > size_) {
        if (error) *error = "data for '" + name + "' lies outside the file";
        return false;
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(entry.compressed_size));
    if (!raw.empty() && !ReadAt(data_offset, raw.data(), raw.size())) {
        if (error) *error = "short read for '" + name + "'";
        return false;
    }

    if (entry.method == 0) {
        *out = std::move(raw);
        return true;
    }
    if (entry.method != 8) {
        if (error) {
            *error = "entry '" + name + "' uses unsupported compression method " +
                     std::to_string(entry.method);
        }
        return false;
    }

    out->assign(static_cast<std::size_t>(entry.size), 0);
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        if (error) *error = "zlib refused to initialise";
        return false;
    }
    stream.next_in = raw.data();
    stream.avail_in = static_cast<uInt>(raw.size());
    stream.next_out = out->data();
    stream.avail_out = static_cast<uInt>(out->size());
    const int status = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (status != Z_STREAM_END || stream.total_out != out->size()) {
        if (error) *error = "cannot inflate '" + name + "'";
        return false;
    }
    return true;
}

bool GuestApk::Open(const std::string& path, std::string* error) {
    path_ = path;
    abi_directory_.clear();
    entries_.clear();
    index_.clear();
    directories_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        if (error) *error = "cannot open '" + path + "': " + strerror(errno);
        return false;
    }
    struct stat info {};
    if (::fstat(fd_, &info) != 0) {
        if (error) *error = "cannot measure '" + path + "': " + strerror(errno);
        return false;
    }
    size_ = static_cast<std::uint64_t>(info.st_size);
    if (size_ < kEndOfCentralDirectorySize) {
        if (error) *error = "'" + path + "' is too small to be an APK";
        return false;
    }

    // The end record sits at the very end, possibly followed by a comment of up
    // to 64 KiB, so only that tail is read. An archive can be hundreds of
    // megabytes, and reading the whole thing to find a 22-byte record copied the
    // file once per start.
    const std::uint64_t tail_size =
        std::min<std::uint64_t>(size_, kMaxComment + kEndOfCentralDirectorySize);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    if (!ReadAt(size_ - tail_size, tail.data(), tail.size())) {
        if (error) *error = "cannot read the end of '" + path + "'";
        return false;
    }

    std::size_t end = 0;
    bool found = false;
    for (std::size_t offset = tail.size() - kEndOfCentralDirectorySize;; --offset) {
        if (Read32(tail.data(), offset) == kEndOfCentralDirectory) {
            end = offset;
            found = true;
            break;
        }
        if (offset == 0) {
            break;
        }
    }
    if (!found) {
        if (error) *error = "'" + path + "' has no zip end record";
        return false;
    }

    const std::uint32_t directory_size = Read32(tail.data(), end + 12);
    const std::uint32_t directory_offset = Read32(tail.data(), end + 16);
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > size_) {
        if (error) *error = "central directory lies outside the file";
        return false;
    }

    // Only the central directory is read, and the walk is bounded by its own
    // length: the entry count in the end record is 16-bit and lies about large
    // archives.
    std::vector<std::uint8_t> directory(directory_size);
    if (!directory.empty() &&
        !ReadAt(directory_offset, directory.data(), directory.size())) {
        if (error) *error = "cannot read the central directory";
        return false;
    }

    std::size_t cursor = 0;
    while (cursor + 46 <= directory.size() &&
           Read32(directory.data(), cursor) == kCentralHeader) {
        Entry entry;
        entry.method = Read16(directory.data(), cursor + 10);
        entry.compressed_size = Read32(directory.data(), cursor + 20);
        entry.size = Read32(directory.data(), cursor + 24);
        const std::uint16_t name_length = Read16(directory.data(), cursor + 28);
        const std::uint16_t extra_length = Read16(directory.data(), cursor + 30);
        const std::uint16_t comment_length = Read16(directory.data(), cursor + 32);
        entry.local_offset = Read32(directory.data(), cursor + 42);
        if (cursor + 46 + name_length > directory.size()) {
            if (error) *error = "entry name lies outside the central directory";
            return false;
        }
        entry.name.assign(reinterpret_cast<const char*>(directory.data() + cursor + 46),
                          name_length);

        index_[entry.name] = entries_.size();
        // The immediate children of its parent directory: a directory listing is
        // then one lookup instead of a scan of every entry in the archive. An
        // entry that is itself a directory ("a/b/") has no name after the slash
        // and is only a parent, not a child of anyone. Directories are not
        // listed to the guest -- AAssetDir_getNextFileName skips everything that
        // is not a regular file -- so an implicit parent stays invisible.
        const std::size_t slash = entry.name.rfind('/');
        if (slash != std::string::npos && slash + 1 < entry.name.size()) {
            directories_[entry.name.substr(0, slash)].push_back(
                entry.name.substr(slash + 1));
        }
        entries_.push_back(std::move(entry));
        cursor += 46 + name_length + extra_length + comment_length;
    }
    if (entries_.empty()) {
        if (error) *error = "'" + path + "' has an empty central directory";
        return false;
    }

    // Archive order is not a listing order: a file may appear anywhere in the
    // central directory. Sorting also makes the duplicates that a directory's
    // own entry produces collapse.
    for (auto& [parent, children] : directories_) {
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
    }
    // The platform picks exactly one ABI directory per application: armeabi-v7a
    // when the archive has one, armeabi otherwise. It never mixes them, and an
    // archive that carries an empty armeabi-v7a directory is an archive without
    // v7a libraries.
    for (const char* candidate : {"lib/armeabi-v7a/", "lib/armeabi/"}) {
        const std::size_t length = std::strlen(candidate);
        const bool found = std::any_of(entries_.begin(), entries_.end(), [&](const Entry& entry) {
            return entry.name.compare(0, length, candidate) == 0 && EndsWith(entry.name, ".so");
        });
        if (found) {
            abi_directory_ = candidate;
            break;
        }
    }
    return true;
}

const GuestApk::Entry* GuestApk::Find(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &entries_[it->second];
}

std::vector<std::string> GuestApk::NativeLibraries() const {
    std::vector<std::string> names;
    if (abi_directory_.empty()) {
        return names;
    }
    for (const Entry& entry : entries_) {
        if (entry.name.rfind(abi_directory_, 0) != 0) {
            continue;
        }
        if (!EndsWith(entry.name, ".so")) {
            continue;
        }
        names.push_back(entry.name.substr(abi_directory_.size()));
    }
    return names;
}

bool GuestApk::Read(const std::string& name, std::vector<std::uint8_t>* out,
                    std::string* error) const {
    const Entry* entry = Find(name);
    if (entry == nullptr) {
        if (error) *error = "'" + name + "' is not in the archive";
        return false;
    }
    return ExtractEntry(*entry, name, out, error);
}

bool GuestApk::ReadAsset(const std::string& name, std::vector<std::uint8_t>* out,
                         std::string* error) const {
    std::string relative = AssetRelative(name);
    if (relative.empty()) {
        if (error) *error = "empty asset name";
        return false;
    }
    if (relative.rfind("assets/", 0) != 0) {
        relative.insert(0, "assets/");
    }
    const Entry* entry = Find(relative);
    if (entry == nullptr) {
        if (error) *error = "'" + relative + "' is not in the archive";
        return false;
    }
    return ExtractEntry(*entry, relative, out, error);
}

std::vector<std::string> GuestApk::ListAssets(const std::string& directory) const {
    std::string prefix = AssetRelative(directory);
    if (prefix.rfind("assets/", 0) != 0) {
        prefix.insert(0, "assets/");
    }
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
    const auto it = directories_.find(prefix);
    if (it == directories_.end()) {
        return {};
    }
    return it->second;
}

bool GuestApk::Extract(const std::string& name, const std::string& host_path,
                       std::string* error) const {
    std::vector<std::uint8_t> bytes;
    if (!Read(name, &bytes, error)) {
        return false;
    }
    std::FILE* file = std::fopen(host_path.c_str(), "wb");
    if (file == nullptr) {
        if (error) *error = "cannot create '" + host_path + "'";
        return false;
    }
    const bool written =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    if (!written) {
        if (error) *error = "short write to '" + host_path + "'";
        return false;
    }
    return true;
}

bool GuestApk::LoadNativeLibraries(DynamicLoader& dynamic, Elf32Loader& loader,
                                   const std::string& directory,
                                   std::vector<LoadedNative>* loaded,
                                   std::string* error) const {
    if (loaded != nullptr) {
        loaded->clear();
    }

    struct Pending {
        std::string name;      // entry name inside the APK
        std::string soname;
        std::string path;      // host path after extraction
        std::vector<std::string> needed;
    };

    std::vector<Pending> pending;
    for (const std::string& library : NativeLibraries()) {
        Pending entry;
        entry.name = abi_directory_ + library;
        entry.soname = library;
        entry.path = directory + "/" + library;

        std::vector<std::uint8_t> image;
        if (!Read(entry.name, &image, error)) {
            return false;
        }
        ImageIdentity identity;
        if (ReadImageIdentity(image.data(), image.size(), &identity)) {
            if (!identity.soname.empty()) {
                entry.soname = identity.soname;
            }
            entry.needed = std::move(identity.needed);
        }
        if (!Extract(entry.name, entry.path, error)) {
            return false;
        }
        pending.push_back(std::move(entry));
    }

    // The whole set is declared before anything is mapped, so a DT_NEEDED entry
    // that names a library of this application passes the loader's check.
    std::set<std::string, std::less<>> own;
    for (const Pending& entry : pending) {
        own.insert(entry.soname);
        loader.AddVirtualLibrary(entry.soname);
    }

    // Repeated passes instead of a topological sort: the dependency graph is a
    // handful of nodes, and a library that still cannot be satisfied after the
    // last pass names exactly what is missing.
    std::vector<bool> done(pending.size(), false);
    std::string last_error;
    bool failed = false;
    std::size_t remaining = pending.size();
    while (remaining != 0) {
        bool progress = false;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (done[index]) {
                continue;
            }
            bool ready = true;
            for (const std::string& dependency : pending[index].needed) {
                const bool from_this_set = own.count(dependency) != 0;
                if (!from_this_set) {
                    continue;  // system libraries are virtual already
                }
                bool satisfied = false;
                if (loaded != nullptr) {
                    for (const LoadedNative& item : *loaded) {
                        if (item.soname == dependency) {
                            satisfied = true;
                            break;
                        }
                    }
                }
                if (!satisfied) {
                    ready = false;
                    break;
                }
            }
            if (!ready) {
                continue;
            }

            std::string load_error;
            const std::uint32_t base = dynamic.Open(pending[index].path, &load_error);
            if (base == 0) {
                last_error = pending[index].soname + ": " + load_error;
                failed = true;
                done[index] = true;  // do not retry forever
                --remaining;
                continue;
            }
            done[index] = true;
            --remaining;
            progress = true;
            if (loaded != nullptr) {
                loaded->push_back(LoadedNative{pending[index].soname, base});
            }
        }
        if (!progress) {
            if (error) {
                *error = last_error.empty() ? "native library dependency cycle" : last_error;
            }
            return false;
        }
    }
    if (failed) {
        if (error) {
            *error = last_error;
        }
        return false;
    }
    return true;
}

bool GuestApk::ExtractNativeLibraries(const std::string& directory,
                                      std::vector<ExtractedNative>* libraries,
                                      std::string* error) const {
    if (libraries != nullptr) {
        libraries->clear();
    }
    ::mkdir(directory.c_str(), 0755);
    for (const std::string& library : NativeLibraries()) {
        const std::string entry_name = abi_directory_ + library;
        std::vector<std::uint8_t> image;
        if (!Read(entry_name, &image, error)) {
            return false;
        }
        ExtractedNative extracted;
        extracted.file = library;
        extracted.soname = library;
        ImageIdentity identity;
        if (ReadImageIdentity(image.data(), image.size(), &identity)) {
            if (!identity.soname.empty()) {
                extracted.soname = identity.soname;
            }
            extracted.needed = std::move(identity.needed);
        }
        if (!Extract(entry_name, directory + "/" + library, error)) {
            return false;
        }
        if (libraries != nullptr) {
            libraries->push_back(std::move(extracted));
        }
    }
    return true;
}

}  // namespace armbat
