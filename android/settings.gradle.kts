/*
 * settings.gradle.kts: Gradle settings for the Android provisioning app.
 *
 * Declares where plugins and libraries are downloaded from and which modules
 * make up the build (just ":app"). See docs/DESIGN.md §9 (Android app).
 */

pluginManagement {
    repositories {
        // Android Gradle Plugin and AndroidX plugins
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    // Every repository must be declared here; a module that adds its own fails the build.
    // That keeps dependency sources in one auditable place.
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        // JitPack builds GitHub projects on demand. Espressif publishes
        // esp-idf-provisioning-android only there (decision D1/D2 in DESIGN.md).
        // Some of its transitive dependencies (e.g. code-scanner) also come from JitPack.
        maven { url = uri("https://jitpack.io") }
    }
}

rootProject.name = "BleWifiProvisioning"
include(":app")
