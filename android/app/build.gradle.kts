/*
 * app/build.gradle.kts: build configuration for the provisioning app module.
 *
 * Architecture reference: docs/DESIGN.md §9 (stack, permissions, screens).
 */
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    // Kotlin package root and the generated R class package.
    namespace = "com.blewifiprov.app"

    // 35 is the minimum the Espressif library (lib-2.4.4) and its CameraX
    // dependency compile against. AGP downloads the SDK platform if it's missing.
    compileSdk = 35

    defaultConfig {
        applicationId = "com.blewifiprov.app"
        // Android 8.0: the Espressif library's documented minimum. Covers
        // almost all active devices (docs/DESIGN.md §9).
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            // v1 ships without code shrinking. Enable R8 once proguard rules for
            // the Espressif library (protobuf-lite, EventBus reflection) are tested.
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        // Java 17 bytecode: required by AGP 8.x and matches the Kotlin jvmTarget.
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    packaging {
        resources {
            // Several transitive libraries (Tink, protobuf) ship the same license
            // files. Drop the duplicates so packaging doesn't fail.
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    // --- Espressif provisioning (BLE transport, Security 0/1/2, protobuf messages) ---
    implementation(libs.esp.provisioning)
    // The library posts DeviceConnectionEvent through EventBus. We subscribe
    // in ProvisioningRepository, so it must be on our compile classpath.
    implementation(libs.eventbus)

    // --- Kotlin / AndroidX ---
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)     // collectAsStateWithLifecycle
    implementation(libs.androidx.lifecycle.viewmodel.compose)   // viewModel() in composables
    implementation(libs.androidx.navigation.compose)

    // --- Jetpack Compose UI (versions come from the BOM) ---
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons.extended)
    debugImplementation(libs.androidx.compose.ui.tooling)       // @Preview rendering in Android Studio
}
