plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// -Pemerald3dsLib=<dir> packages <dir>/armeabi-v7a/libemerald3ds.so (the real engine) and skips the CMake
// build of the self-test host in ../host_test. Without it the self-test library is built.
val prebuiltLib = (project.findProperty("emerald3dsLib") as String?)?.let { file(it) }
val prebuiltAssets = (project.findProperty("emerald3dsAssets") as String?)?.let { file(it) }
val releaseStore = System.getenv("THOR_KEYSTORE")

android {
    namespace = "com.emerald3ds.thor"
    compileSdk = 35
    ndkVersion = "27.3.13750724"
    defaultConfig {
        applicationId = "com.emerald3ds.thor"
        minSdk = 29
        targetSdk = 34
        versionCode = (project.findProperty("thorVersionCode") as String? ?: "400").toInt()
        versionName = project.findProperty("thorVersionName") as String? ?: "0.4.0"
        // The engine is a 32-bit-only game: keeping the app 32-bit-only makes Android start it as a 32-bit process.
        ndk { abiFilters += "armeabi-v7a" }
    }
    if (prebuiltLib == null) {
        externalNativeBuild { cmake { path = file("../host_test/CMakeLists.txt"); version = "3.31.6" } }
    } else {
        sourceSets["main"].jniLibs.srcDir(prebuiltLib)
    }
    if (prebuiltAssets != null) sourceSets["main"].assets.setSrcDirs(listOf(prebuiltAssets))
    if (releaseStore != null) {
        signingConfigs.create("thorRelease") {
            storeFile = file(releaseStore)
            storePassword = System.getenv("THOR_STORE_PASSWORD") ?: error("THOR_STORE_PASSWORD is required")
            keyAlias = System.getenv("THOR_KEY_ALIAS") ?: error("THOR_KEY_ALIAS is required")
            keyPassword = System.getenv("THOR_KEY_PASSWORD") ?: error("THOR_KEY_PASSWORD is required")
        }
        buildTypes.getByName("release").signingConfig = signingConfigs.getByName("thorRelease")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    // Debug uses Android's development key. Production signing credentials come only from the environment.
}
kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }
dependencies { testImplementation("junit:junit:4.13.2") }
