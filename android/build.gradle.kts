/*
 * build.gradle.kts (root): build configuration shared by all modules.
 *
 * Plugins are only *declared* here (`apply false`), with versions from
 * gradle/libs.versions.toml. Each module applies the plugins it needs;
 * see app/build.gradle.kts.
 */
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.android) apply false
    // Kotlin 2.x ships the Compose compiler as a Kotlin plugin.
    alias(libs.plugins.kotlin.compose) apply false
}
