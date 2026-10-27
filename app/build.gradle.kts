plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "dev.armbat"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "29.0.13113456"

    defaultConfig {
        applicationId = "dev.armbat"
        minSdk = 31
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                targets += listOf("armbat_jni", "guestsdk")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_ARM_NEON=ON",
                    "-DARMBAT_GUEST_RUNTIME_BIONIC=ON",
                    // The runtime reports the APK version in its log; Gradle owns
                    // the number so there is no second copy to drift.
                    "-DARMBAT_VERSION_NAME=$versionName",
                    "-DARMBAT_VERSION_CODE=$versionCode",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }

    // The container's android.app.NativeActivity is compiled and dexed on its own
    // (see dexGuestPlatform below): the class has to be defined by the guest
    // class loader and has to keep the platform name, so it cannot live in the
    // application's own dex.
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("platform").get().asFile)

    signingConfigs {
        // The upload key comes from the environment and is never in the tree.
        // Without it the release build is left unsigned instead of failing.
        val keystore = System.getenv("ANDROID_KEYSTORE")
        if (keystore != null) {
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KEYSTORE_ALIAS")
                keyPassword = System.getenv("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

// The guest's window manager inherits from the platform's WindowManagerImpl so
// that the framework can still cast it in Window.setWindowManager. That class is
// public at runtime but hidden from android.jar, so its declaration is compiled
// here for the compiler only and never packaged into the APK.
val compileHiddenPlatformStubs = tasks.register<JavaCompile>("compileHiddenPlatformStubs") {
    source = fileTree("stubs") { include("**/*.java") }
    classpath = files(androidComponents.sdkComponents.bootClasspath)
    destinationDirectory.set(layout.buildDirectory.dir("hidden-platform-stubs/classes"))
    options.compilerArgs.add("-nowarn")
}

val hiddenPlatformStubs = tasks.register<Jar>("hiddenPlatformStubs") {
    from(compileHiddenPlatformStubs.map { it.destinationDirectory })
    archiveFileName.set("hidden-platform-stubs.jar")
    destinationDirectory.set(layout.buildDirectory.dir("hidden-platform-stubs"))
}

dependencies {
    compileOnly(files(hiddenPlatformStubs))
    val composeBom = platform("androidx.compose:compose-bom:2025.09.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.10.0")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.core:core-ktx:1.13.1")
    // Rebuilds an archive's dex with its hardcoded storage paths replaced;
    // see GuestDex for why a byte patch is not enough.
    implementation("com.android.tools.smali:smali-dexlib2:3.0.3")
    implementation("com.google.guava:guava:32.0.1-android")
    testImplementation("junit:junit:4.13.2")
}

// platform/src is compiled against android.jar and dexed separately: the guest
// class loader must define android.app.NativeActivity itself, and the class has
// to carry the platform name, so it cannot ship in the application's own dex.
val platformClasses = layout.buildDirectory.dir("platform-classes")

val compileGuestPlatform = tasks.register<JavaCompile>("compileGuestPlatform") {
    source = rootProject.fileTree("platform/src") { include("**/*.java") }
    classpath = files(androidComponents.sdkComponents.bootClasspath)
    destinationDirectory.set(platformClasses)
    options.release.set(17)
}

val dexGuestPlatform = tasks.register<JavaExec>("dexGuestPlatform") {
    dependsOn(compileGuestPlatform)

    val outputDir = layout.buildDirectory.dir("platform")
    val d8Jar = androidComponents.sdkComponents.sdkDirectory.map { sdk ->
        sdk.file("build-tools/${android.buildToolsVersion}/lib/d8.jar")
    }

    classpath = files(d8Jar)
    mainClass.set("com.android.tools.r8.D8")
    inputs.dir(platformClasses)
    outputs.file(outputDir.map { it.file("guestplatform.dex") })

    doFirst {
        val out = outputDir.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        val androidJar = androidComponents.sdkComponents.bootClasspath.get()
            .map { it.asFile }
            .first { it.name == "android.jar" }
        args = listOf(
            "--release", "--min-api", "31",
            "--lib", androidJar.absolutePath,
            "--output", out.absolutePath,
        ) + fileTree(platformClasses.get()).files.map { it.absolutePath }
    }

    doLast {
        val dex = outputDir.get().file("classes.dex").asFile
        check(dex.renameTo(outputDir.get().file("guestplatform.dex").asFile)) {
            "d8 did not produce classes.dex"
        }
    }
}

tasks.named("preBuild").configure {
    dependsOn(dexGuestPlatform)
}
