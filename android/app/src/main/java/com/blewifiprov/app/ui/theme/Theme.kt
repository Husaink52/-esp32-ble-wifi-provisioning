/*
 * Theme.kt: the app's Material 3 theme.
 *
 * Architecture: UI layer (docs/DESIGN.md §9). On Android 12+ the colours come
 * from the user's wallpaper (dynamic color). Older versions use the fixed blue
 * palette below, which matches the device's blue "provisioning" LED.
 */
package com.blewifiprov.app.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

/** Fallback light palette (Android 8–11). */
private val LightColors = lightColorScheme(
    primary = Color(0xFF1565C0),
    secondary = Color(0xFF00897B),
)

/** Fallback dark palette (Android 8–11). */
private val DarkColors = darkColorScheme(
    primary = Color(0xFF90CAF9),
    secondary = Color(0xFF80CBC4),
)

/** Colour used for success states (e.g. "All set!"); green like the device LED. */
val SuccessGreen = Color(0xFF2E7D32)

/**
 * Wrap all app content in this theme.
 *
 * @param darkTheme Follow the system dark-mode setting by default.
 * @param content The composable content to theme.
 */
@Composable
fun BleProvTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val colorScheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
        }
        darkTheme -> DarkColors
        else -> LightColors
    }
    MaterialTheme(colorScheme = colorScheme, content = content)
}
