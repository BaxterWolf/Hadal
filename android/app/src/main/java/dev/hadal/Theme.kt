package dev.hadal

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.random.Random

// Colours
val HadalColors = darkColorScheme(
    primary = Color(0xFFC9A45C), onPrimary = Color(0xFF1B1308),
    primaryContainer = Color(0xFF3A2E1B), onPrimaryContainer = Color(0xFFF0DCB0),
    secondary = Color(0xFF7FC7B0), onSecondary = Color(0xFF052019),
    secondaryContainer = Color(0xFF14303A), onSecondaryContainer = Color(0xFFD3ECE4),
    tertiary = Color(0xFFFF4A3D), onTertiary = Color(0xFF2B0503),
    tertiaryContainer = Color(0xFF4A1512), onTertiaryContainer = Color(0xFFFFD9D4),
    background = Color(0xFF03070B), onBackground = Color(0xFFDCE8E4),
    surface = Color(0xFF060D12), onSurface = Color(0xFFDCE8E4),
    surfaceVariant = Color(0xFF14262D), onSurfaceVariant = Color(0xFF95ADA7),
    surfaceContainerLowest = Color(0xFF03070B), surfaceContainerLow = Color(0xFF08131A), surfaceContainer = Color(0xFF0B1920),
    surfaceContainerHigh = Color(0xFF102129), surfaceContainerHighest = Color(0xFF152A33),
    outline = Color(0xFF2F4A4F), outlineVariant = Color(0xFF1C3036),
    inverseSurface = Color(0xFFE8DCC0), inverseOnSurface = Color(0xFF1B1308), inversePrimary = Color(0xFF6B5230),
    error = Color(0xFFFF6B5E), onError = Color(0xFF2B0503), errorContainer = Color(0xFF4A1512), onErrorContainer = Color(0xFFFFD9D4),
)

// Fonts
val HadalType = Typography().run {
    val plate = FontFamily.Serif
    copy(
        headlineMedium = headlineMedium.copy(fontFamily = plate, fontWeight = FontWeight.Medium, letterSpacing = 8.sp),
        headlineSmall = headlineSmall.copy(fontFamily = plate, fontWeight = FontWeight.Medium, letterSpacing = 3.sp),
        titleMedium = titleMedium.copy(fontWeight = FontWeight.Medium),
    )
}
val Gauge = TextStyle(fontFamily = FontFamily.Monospace)
val Plate = TextStyle(fontFamily = FontFamily.Serif, fontWeight = FontWeight.Medium, letterSpacing = 2.sp)

@Composable
fun HadalTheme(content: @Composable () -> Unit) = MaterialTheme(colorScheme = HadalColors, typography = HadalType) {
    CompositionLocalProvider(LocalContentColor provides HadalColors.onBackground) { Abyss(content) }
}

@Composable
fun Bar(fraction: Float, modifier: Modifier = Modifier, color: Color = MaterialTheme.colorScheme.primary) = LinearProgressIndicator(
    progress = { fraction.coerceIn(0f, 1f) }, modifier = modifier, color = color,
    trackColor = MaterialTheme.colorScheme.surfaceContainerHighest, gapSize = 0.dp, drawStopIndicator = {},
)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HadalSlider(
    value: Float, onValueChange: (Float) -> Unit, modifier: Modifier = Modifier, enabled: Boolean = true,
    valueRange: ClosedFloatingPointRange<Float> = 0f..1f, onValueChangeFinished: (() -> Unit)? = null,
) = Slider(
    value, onValueChange, modifier, enabled = enabled, valueRange = valueRange, onValueChangeFinished = onValueChangeFinished,
    track = { SliderDefaults.Track(sliderState = it, enabled = enabled, drawStopIndicator = null) },
)

@Composable
fun Abyss(content: @Composable () -> Unit) {
    val snow = remember { Random(7).let { r -> List(70) { Triple(r.nextFloat(), r.nextFloat(), r.nextFloat()) } } }
    Box(
        Modifier.fillMaxSize()
            .background(Brush.verticalGradient(listOf(Color(0xFF17332C), Color(0xFF0B1F29), Color(0xFF03070B))))
            .drawBehind {
                for ((x, y, k) in snow)
                    drawCircle(Color(0xFFCFE8DF), radius = (0.6f + k * 1.1f) * density, center = Offset(x * size.width, y * size.height),
                        alpha = (0.05f + k * 0.2f) * (1f - y * 0.7f))
            },
    ) { content() }
}

@Composable
fun hadalSegments() = SegmentedButtonDefaults.colors(
    activeContainerColor = MaterialTheme.colorScheme.primary,
    activeContentColor = MaterialTheme.colorScheme.onPrimary,
    inactiveContainerColor = MaterialTheme.colorScheme.secondaryContainer,
    inactiveContentColor = MaterialTheme.colorScheme.onSecondaryContainer,
    activeBorderColor = MaterialTheme.colorScheme.surface,
    inactiveBorderColor = MaterialTheme.colorScheme.surface,
)

@Composable
fun Panel(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) = Card(
    modifier,
    shape = RoundedCornerShape(22.dp),
    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.82f), contentColor = MaterialTheme.colorScheme.onSurface),
    border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
    content = content,
)
