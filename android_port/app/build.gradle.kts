plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// -Pemerald3dsLib=<dir> packages <dir>/armeabi-v7a/libemerald3ds.so (the real engine) and skips the CMake
// build of the self-test host in ../host_test. Without it the self-test library is built.
val prebuiltLib = (project.findProperty("emerald3dsLib") as String?)?.let { file(it) }

android {
    namespace = "com.emerald3ds.thor"
    compileSdk = 35
    ndkVersion = "27.3.13750724"
    defaultConfig {
        applicationId = "com.emerald3ds.thor"
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-dev"
        // The engine is a 32-bit-only game: keeping the app 32-bit-only makes Android start it as a 32-bit process.
        ndk { abiFilters += "armeabi-v7a" }
    }
    if (prebuiltLib == null) {
        externalNativeBuild { cmake { path = file("../host_test/CMakeLists.txt"); version = "3.31.6" } }
    } else {
        sourceSets["main"].jniLibs.srcDir(prebuiltLib)
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    // The debug keystore Gradle creates in ~/.android signs every build; no key material lives in the repo.
}
kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }
