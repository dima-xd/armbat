package dev.armbat

import java.io.File

/**
 * The shared-storage view a guest gets.
 *
 * An archive asks the platform for the external storage root and then builds
 * its own paths from it: `.../Android/obb/<package>/main.<n>.obb` for expansion
 * files, `.../Android/data/<package>/...` for its external files. On a device
 * that root is the real `/storage/emulated/0`, where those paths belong to no
 * application (and are unreadable on a current release). The container keeps a
 * guest's tree under its own [GuestStorage.root], so the root handed to the
 * guest is that one -- the same place its `getExternalFilesDir` and `getObbDir`
 * already point at.
 *
 * [GuestDex] redirects the corresponding `Environment` calls here.
 */
object GuestEnvironment {

    @JvmStatic
    fun getExternalStorageDirectory(): File = GuestStorage.root()

    @JvmStatic
    fun getExternalStoragePublicDirectory(type: String): File = File(GuestStorage.root(), type)
}
