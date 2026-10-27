#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace armbat {

class DynamicLoader;
class Elf32Loader;

// The parts of an APK the container needs before anything runs.
//
// An APK is a zip file with a manifest and a dex, and the native libraries an
// application ships for each ABI under lib/<abi>/. Armbat only ever wants the
// ARMv7 entries: the rest are somebody else's business, and loading an arm64
// library as if it were ARMv7 would fail in a far more confusing place.
class GuestApk {
public:
    struct Entry {
        std::string name;
        std::uint64_t size = 0;           // uncompressed
        std::uint64_t compressed_size = 0;
        std::uint64_t local_offset = 0;   // start of the entry's local header
        std::uint16_t method = 0;         // 0 stored, 8 deflated
    };

    GuestApk() = default;
    ~GuestApk();
    GuestApk(const GuestApk&) = delete;
    GuestApk& operator=(const GuestApk&) = delete;

    bool Open(const std::string& path, std::string* error);
    const std::string& Path() const { return path_; }

    // Every entry in the archive, in central-directory order.
    const std::vector<Entry>& Entries() const { return entries_; }

    // The ABI directory the archive's libraries live in, with its trailing
    // slash, or empty when the archive ships no arm32 library.
    //
    // Android prefers armeabi-v7a and falls back to armeabi for archives built
    // before that ABI existed. Both run here: armeabi code is ARMv5/ARMv6, and
    // every instruction it uses is a subset of ARMv7. Ignoring lib/armeabi made
    // a whole generation of applications look like pure Java ones, and their
    // System.loadLibrary then failed with a name the archive clearly contains.
    const std::string& AbiDirectory() const { return abi_directory_; }

    // Entries under the archive's ABI directory with a .so extension, as bare
    // names.
    std::vector<std::string> NativeLibraries() const;

    // Reads one entry and decompresses it. Returns false with a reason.
    bool Read(const std::string& name, std::vector<std::uint8_t>* out,
              std::string* error) const;
    bool Extract(const std::string& name, const std::string& host_path,
                 std::string* error) const;

    // Reads an archive asset the way AAssetManager does: a name without a
    // leading slash is relative to assets/, and the archive root is not
    // searched first. Applications ship their whole data tree there, so a
    // lookup of "resource_packs/vanilla/manifest.json" means
    // "assets/resource_packs/vanilla/manifest.json".
    bool ReadAsset(const std::string& name, std::vector<std::uint8_t>* out,
                   std::string* error) const;

    // Immediate children of an asset directory, the names
    // AAssetDir_getNextFileName returns. The directory itself may be given with
    // or without the assets/ prefix.
    std::vector<std::string> ListAssets(const std::string& directory) const;

    // The entry with this name, or nullptr.
    const Entry* Find(const std::string& name) const;

    // Loads the application's own arm32 libraries the way Android would.
    //
    // Every library is extracted to `directory` first, then opened in dependency
    // order: a library that names another library of the same application in
    // DT_NEEDED must find it already loaded, because imports are resolved when
    // the image is mapped, not lazily. Sonames of the whole set are declared
    // virtual up front so the dependency check accepts them, and cycles still
    // fail with the name of the library that could not be satisfied.
    struct LoadedNative {
        std::string soname;
        std::uint32_t base = 0;
    };
    bool LoadNativeLibraries(DynamicLoader& dynamic, Elf32Loader& loader,
                             const std::string& directory, std::vector<LoadedNative>* loaded,
                             std::string* error) const;

    // One extracted library. `file` is the name the archive stores it under and
    // the name it has on disk; `soname` is what the image calls itself in
    // DT_SONAME. The two are not the same field: Halfbrick's libmortargame.so
    // names itself "liblibmortargame" and libMicroMapJNI.so names itself
    // "libMicroMapJNI.so.1.3.0". The loader opens the file, the linker records
    // the soname.
    struct ExtractedNative {
        std::string file;
        std::string soname;
        // The DT_NEEDED entries of the image. The guest's linker resolves them
        // by name, so a sibling has to be loaded before the library that names
        // it; the container opens the dependencies itself, in this order.
        std::vector<std::string> needed;
    };

    // Extracts every arm32 library into `directory` and returns them in archive
    // order. Used by the bionic runtime, where the guest's own linker performs
    // the load and the container only has to provide the files.
    bool ExtractNativeLibraries(const std::string& directory,
                                std::vector<ExtractedNative>* libraries,
                                std::string* error) const;

private:
    // Reads exactly `length` bytes at an absolute offset. pread rather than a
    // shared stream: assets are extracted from several guest threads, and a
    // shared file position would make one thread's read land in another's
    // buffer.
    bool ReadAt(std::uint64_t offset, void* out, std::size_t length) const;

    // The entry's data, decompressed, with the local header parsed on the way:
    // the local header repeats the name and extra lengths, which need not match
    // the central directory's, so the data offset is only known for sure here.
    bool ExtractEntry(const Entry& entry, const std::string& name,
                      std::vector<std::uint8_t>* out, std::string* error) const;

    std::string path_;
    std::string abi_directory_;
    int fd_ = -1;
    std::uint64_t size_ = 0;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    // Immediate children of every directory in the archive, sorted. Pack loading
    // walks thousands of directories, and answering each walk with a scan of all
    // entries was the second most expensive thing the loader did.
    std::unordered_map<std::string, std::vector<std::string>> directories_;
};

}  // namespace armbat
