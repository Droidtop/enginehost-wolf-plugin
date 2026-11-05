plugins { id("com.android.library") }

android {
    namespace = "dev.enginehost.plugin.wolf"
    compileSdk = 36
    defaultConfig {
        minSdk = 26
        // One bundle serves every device: the host picks lib/<abi> out of the
        // bundle from Build.SUPPORTED_ABIS, so both ABIs ship side by side.
        // arm64-v8a is the console; x86_64 is x86 hardware and the Android
        // emulator rig we test on. Nothing in the engine depends on either.
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        // One library and no C++ runtime beside it: the isolated runtime
        // loads the bundle's libraries from descriptors.
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=none" } }
    }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies { compileOnly(project(":api")) }
