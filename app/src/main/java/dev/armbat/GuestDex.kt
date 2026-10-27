package dev.armbat

import android.content.Context
import android.os.SystemClock
import com.android.tools.smali.dexlib2.DexFileFactory
import com.android.tools.smali.dexlib2.Opcode
import com.android.tools.smali.dexlib2.Opcodes
import com.android.tools.smali.dexlib2.iface.DexFile
import com.android.tools.smali.dexlib2.iface.instruction.Instruction
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction21c
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction31c
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction35c
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction3rc
import com.android.tools.smali.dexlib2.iface.instruction.ReferenceInstruction
import com.android.tools.smali.dexlib2.iface.reference.MethodReference
import com.android.tools.smali.dexlib2.iface.reference.StringReference
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction21c
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction31c
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction35c
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction3rc
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableStringReference
import com.android.tools.smali.dexlib2.rewriter.DexRewriter
import com.android.tools.smali.dexlib2.rewriter.InstructionRewriter
import com.android.tools.smali.dexlib2.rewriter.Rewriter
import com.android.tools.smali.dexlib2.rewriter.RewriterModule
import com.android.tools.smali.dexlib2.rewriter.Rewriters
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.ConcurrentHashMap
import java.util.zip.ZipFile

/**
 * Makes the absolute paths an archive hardcodes in its Java code point at the
 * container's own storage.
 *
 * A guest is not an installed package. Its dex runs in this process, so a
 * literal File("/data/data/<package>") looks at a directory that does not
 * exist, and File.list() answers null; an installer that scans its own data
 * folder dies on that before it can say what it needs. Native code does not
 * have the problem: the runtime maps these paths. Java has no such mapper, so
 * the string constants are replaced here.
 *
 * A string is referenced by index, and the dex requires the string table to be
 * sorted, so a longer replacement cannot be written in place. dexlib2 rebuilds
 * the whole file: it collects the new strings, sorts them and remaps every
 * reference that points into the table.
 *
 * Rebuilding a 4 MB dex costs seconds, so the result is cached by the archive's
 * dex checksums and the rewrite inputs. The cache path stays the same across
 * launches, which also lets ART keep its profile and oat data next to the file.
 *
 * The same pass also redirects a few framework calls whose answer belongs to
 * the guest rather than to the container. The framework evaluates a behaviour
 * change (PendingIntent mutability) against the uid's target SDK, which is the
 * container's 36, not the archive's; an archive built before Android 12 passes
 * 0 and the factory throws. And `Environment` describes the real device's
 * shared storage, so an archive builds `Android/obb/...` paths that exist for
 * no package; the guest's tree lives under [GuestStorage]. Both go through
 * helpers in this package ([GuestPendingIntents], [GuestEnvironment]) with the
 * same name and prototype, so the call sites need no other change.
 */
object GuestDex {

    /** Bump when the rewrite rules or the cache layout change. */
    private const val CACHE_VERSION = 3

    private const val CACHE_MARKER = ".ok"
    private const val PLATFORM_DEX = "guestplatform.dex"

    private const val PENDING_INTENT_CLASS = "Landroid/app/PendingIntent;"
    private const val GUEST_PENDING_INTENTS = "Ldev/armbat/GuestPendingIntents;"
    private val PENDING_INTENT_FACTORIES = setOf(
        "getActivity",
        "getActivities",
        "getBroadcast",
        "getService",
        "getForegroundService",
    )

    private const val ENVIRONMENT_CLASS = "Landroid/os/Environment;"
    private const val GUEST_ENVIRONMENT = "Ldev/armbat/GuestEnvironment;"
    private val ENVIRONMENT_METHODS = setOf(
        "getExternalStorageDirectory",
        "getExternalStoragePublicDirectory",
    )

    /** One lock per archive root, so a launch and import prewarm cannot race. */
    private val locks = ConcurrentHashMap<String, Any>()

    private data class DexEntry(val name: String, val crc: Long, val size: Long)

    /**
     * Extracts the archive's dex files, rewriting the paths only when the
     * string table actually contains one.
     *
     * The result is cached under `files/dex/<apk>/<key>/`; `guestplatform.dex`
     * is part of the same directory because it is loaded through the same
     * class loader and changes with the container build.
     */
    fun prepare(
        host: Context,
        apkPath: String,
        packageName: String,
        data: File,
        obb: File,
        onStage: ((LaunchStage) -> Unit)? = null,
    ): List<File> {
        val apk = File(apkPath)
        val entries = dexEntries(apk)
        if (entries.isEmpty()) {
            return emptyList()
        }
        onStage?.invoke(LaunchStage.DEX)
        val root = File(host.filesDir, "dex/${apk.nameWithoutExtension}")
        val key = cacheKey(host, packageName, data, obb, entries)
        val lock = locks.computeIfAbsent(root.absolutePath) { Any() }
        return synchronized(lock) {
            val directory = File(root, key)
            val names = entries.map { it.name } + PLATFORM_DEX
            val started = SystemClock.elapsedRealtime()
            if (isComplete(directory, names)) {
                names.forEach { name ->
                    val file = File(directory, name)
                    if (file.canWrite()) {
                        file.setReadOnly()
                    }
                }
                ArmbatLog.i(
                    "Armbat",
                    "dex cache: hit ${entries.size} files in ${SystemClock.elapsedRealtime() - started} ms",
                )
                return@synchronized names.map { File(directory, it) }.sortedBy { it.name }
            }
            val files = build(host, apk, packageName, data, obb, root, directory, entries, onStage)
            ArmbatLog.i(
                "Armbat",
                "dex cache: built ${entries.size} files in ${SystemClock.elapsedRealtime() - started} ms",
            )
            files
        }
    }

    /**
     * Rewrites every string constant that names one of the guest's paths.
     * `data` and `obb` are the guest's folders on the shared storage.
     */
    fun rewrite(
        file: File,
        packageName: String,
        data: File,
        obb: File,
        onStage: ((LaunchStage) -> Unit)? = null,
    ) {
        if (packageName.isEmpty()) {
            return
        }
        val rules = rules(packageName, data.absolutePath, obb.absolutePath)
        onStage?.invoke(LaunchStage.DEX_READ)
        if (!GuestDexScan.containsAny(
                file,
                rules.map { it.first } + listOf(PENDING_INTENT_CLASS, ENVIRONMENT_CLASS),
            )
        ) {
            ArmbatLog.i("Armbat", "dex paths: ${file.name} unchanged")
            return
        }
        val dex: DexFile = try {
            DexFileFactory.loadDexFile(file, Opcodes.getDefault())
        } catch (error: Exception) {
            ArmbatLog.w("Armbat", "dex ${file.name} not readable: $error")
            return
        }
        val rewriter = DexRewriter(object : RewriterModule() {
            override fun getInstructionRewriter(rewriters: Rewriters): Rewriter<Instruction> {
                return object : InstructionRewriter(rewriters) {
                    override fun rewrite(instruction: Instruction): Instruction {
                        redirect(instruction)?.let { return it }
                        val path = pathOf(instruction) ?: return super.rewrite(instruction)
                        val replacement = rewritePath(path, rules)
                        if (replacement == path) {
                            return super.rewrite(instruction)
                        }
                        return when (instruction) {
                            is Instruction21c -> ImmutableInstruction21c(
                                Opcode.CONST_STRING,
                                instruction.registerA,
                                ImmutableStringReference(replacement),
                            )
                            is Instruction31c -> ImmutableInstruction31c(
                                Opcode.CONST_STRING_JUMBO,
                                instruction.registerA,
                                ImmutableStringReference(replacement),
                            )
                            else -> super.rewrite(instruction)
                        }
                    }
                }
            }
        })
        onStage?.invoke(LaunchStage.DEX_REWRITE)
        val rewritten = rewriter.dexFileRewriter.rewrite(dex)
        // The loader reads the dex file lazily, so it cannot be replaced while
        // that handle is open: write a sibling and move it over.
        val temporary = File(file.parentFile, "rewritten-${file.name}")
        temporary.delete()
        // The rewriter is lazy: the actual walk and the file write both happen
        // inside writeDexFile, so that call is the long "optimizing" phase.
        DexFileFactory.writeDexFile(temporary.absolutePath, rewritten)
        onStage?.invoke(LaunchStage.DEX_WRITE)
        if (!temporary.renameTo(file)) {
            file.delete()
            temporary.renameTo(file)
        }
        ArmbatLog.i("Armbat", "dex paths: ${file.name} rewritten")
    }

    private fun dexEntries(apk: File): List<DexEntry> = ZipFile(apk).use { zip ->
        zip.entries().asSequence()
            .filter { it.name.startsWith("classes") && it.name.endsWith(".dex") && !it.name.contains('/') }
            .sortedBy { it.name }
            .map { DexEntry(it.name, it.crc, it.size) }
            .toList()
    }

    private fun cacheKey(
        host: Context,
        packageName: String,
        data: File,
        obb: File,
        entries: List<DexEntry>,
    ): String {
        val hostApk = File(host.applicationInfo.sourceDir)
        val raw = buildString {
            append(CACHE_VERSION).append('|')
            append(packageName).append('|')
            append(data.absolutePath).append('|')
            append(obb.absolutePath).append('|')
            append(hostApk.length()).append('|')
            append(hostApk.lastModified()).append('|')
            for (entry in entries) {
                append(entry.name).append(':').append(entry.crc).append(':').append(entry.size).append('|')
            }
        }
        val digest = MessageDigest.getInstance("SHA-256").digest(raw.toByteArray(Charsets.UTF_8))
        return buildString {
            for (byte in digest) {
                append("%02x".format(byte.toInt() and 0xFF))
            }
        }.take(32)
    }

    private fun isComplete(directory: File, names: List<String>): Boolean =
        directory.isDirectory &&
            File(directory, CACHE_MARKER).isFile &&
            names.all { File(directory, it).isFile }

    private fun build(
        host: Context,
        apk: File,
        packageName: String,
        data: File,
        obb: File,
        root: File,
        directory: File,
        entries: List<DexEntry>,
        onStage: ((LaunchStage) -> Unit)?,
    ): List<File> {
        val temporary = File(root, ".tmp-${System.nanoTime()}")
        temporary.deleteRecursively()
        if (!temporary.mkdirs()) {
            throw IOException(host.getString(R.string.error_dex_create, temporary.path))
        }
        try {
            ZipFile(apk).use { zip ->
                for (entry in entries) {
                    val target = File(temporary, entry.name)
                    val source = zip.getEntry(entry.name)
                        ?: throw IOException(host.getString(R.string.error_dex_missing, entry.name))
                    zip.getInputStream(source).use { input ->
                        target.outputStream().use { output -> input.copyTo(output) }
                    }
                    rewrite(target, packageName, data, obb, onStage)
                    target.setReadOnly()
                }
            }
            val platform = File(temporary, PLATFORM_DEX)
            host.assets.open(PLATFORM_DEX).use { input ->
                platform.outputStream().use { output -> input.copyTo(output) }
            }
            platform.setReadOnly()
            // The marker is written last: a crash in the middle leaves a
            // directory that the next launch deletes instead of loading.
            File(temporary, CACHE_MARKER).writeText("")
            directory.deleteRecursively()
            if (!temporary.renameTo(directory)) {
                directory.deleteRecursively()
                if (!temporary.renameTo(directory)) {
                    throw IOException(host.getString(R.string.error_dex_publish, directory.path))
                }
            }
            root.listFiles()?.forEach { sibling ->
                if (sibling != directory) {
                    sibling.deleteRecursively()
                }
            }
            return (entries.map { File(directory, it.name) } + File(directory, PLATFORM_DEX))
                .sortedBy { it.name }
        } catch (error: Throwable) {
            temporary.deleteRecursively()
            throw error
        }
    }

    /** The value of a const-string/const-string/jumbo, or null for anything else. */
    private fun pathOf(instruction: Instruction): String? {
        if (instruction is Instruction21c && instruction.opcode == Opcode.CONST_STRING) {
            return (instruction.reference as? StringReference)?.string
        }
        if (instruction is Instruction31c && instruction.opcode == Opcode.CONST_STRING_JUMBO) {
            return (instruction.reference as? StringReference)?.string
        }
        return null
    }

    /**
     * Redirects a framework call whose answer must describe the guest.
     *
     * PendingIntent: the archive predates explicit mutability, the framework
     *   reads the requirement from the process (the container's current SDK)
     *   and the factory throws before the guest sees an object, so
     *   [GuestPendingIntents] supplies the flags.
     * Environment: the shared storage root is the real device's, so paths the
     *   archive builds from it name no package; [GuestEnvironment] answers
     *   with the guest's tree instead.
     *
     * The replacement keeps the same name and prototype, so the call site
     * needs no other change.
     */
    private fun redirect(instruction: Instruction): Instruction? {
        val reference = (instruction as? ReferenceInstruction)?.reference
            as? MethodReference ?: return null
        val helper = when (reference.definingClass) {
            PENDING_INTENT_CLASS ->
                GUEST_PENDING_INTENTS.takeIf { reference.name in PENDING_INTENT_FACTORIES }
            ENVIRONMENT_CLASS ->
                GUEST_ENVIRONMENT.takeIf { reference.name in ENVIRONMENT_METHODS }
            else -> null
        } ?: return null
        val target = ImmutableMethodReference(
            helper,
            reference.name,
            reference.parameterTypes,
            reference.returnType,
        )
        return when (instruction) {
            is Instruction35c -> ImmutableInstruction35c(
                instruction.opcode,
                instruction.registerCount,
                instruction.registerC,
                instruction.registerD,
                instruction.registerE,
                instruction.registerF,
                instruction.registerG,
                target,
            )
            is Instruction3rc -> ImmutableInstruction3rc(
                instruction.opcode,
                instruction.registerCount,
                instruction.startRegister,
                target,
            )
            else -> null
        }
    }

    private fun rewritePath(value: String, rules: List<Pair<String, String>>): String {
        for ((from, to) in rules) {
            if (!value.startsWith(from)) {
                continue
            }
            // A package path ends at a separator, not in the middle of a
            // longer name.
            if (value.length > from.length && value[from.length] != '/') {
                continue
            }
            return to + value.substring(from.length)
        }
        return value
    }

    private fun rules(packageName: String, data: String, obb: String): List<Pair<String, String>> {
        val external = GuestStorage.root().parentFile?.absolutePath ?: "/storage/emulated/0"
        return listOf(
            "/storage/emulated/0/Android/data/$packageName" to data,
            "/mnt/sdcard/Android/data/$packageName" to data,
            "/sdcard/Android/data/$packageName" to data,
            "/data/user/0/$packageName" to data,
            "/data/data/$packageName" to data,
            "/storage/emulated/0/Android/obb/$packageName" to obb,
            "/mnt/sdcard/Android/obb/$packageName" to obb,
            "/sdcard/Android/obb/$packageName" to obb,
            "/storage/sdcard0" to external,
            "/storage/sdcard" to external,
            "/mnt/sdcard" to external,
            "/sdcard" to external,
        )
    }
}
