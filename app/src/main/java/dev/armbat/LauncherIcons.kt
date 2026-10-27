package dev.armbat

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate

/**
 * The launcher's own icons, drawn rather than shipped as bitmaps.
 *
 * The reference is the Android 2.x/4.x launcher: chrome, glass and the green
 * arrow of the era. Drawing them keeps one set for every density, and no asset
 * of the original distribution is copied.
 */

@Composable
fun SearchIcon(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        val lens = s * 0.28f
        val center = Offset(s * 0.42f, s * 0.42f)

        drawLine(
            color = Color(0xFF8FA2B3),
            start = Offset(s * 0.60f, s * 0.60f),
            end = Offset(s * 0.88f, s * 0.88f),
            strokeWidth = s * 0.15f,
            cap = StrokeCap.Round,
        )
        drawLine(
            color = Color(0xFFE4EDF4),
            start = Offset(s * 0.62f, s * 0.62f),
            end = Offset(s * 0.86f, s * 0.86f),
            strokeWidth = s * 0.06f,
            cap = StrokeCap.Round,
        )
        drawCircle(
            brush = Brush.radialGradient(
                colors = listOf(Color(0xFFFBFDFF), Color(0xFFB9CCDC), Color(0xFF6E8598)),
                center = Offset(center.x - lens * 0.45f, center.y - lens * 0.55f),
                radius = lens * 2.1f,
            ),
            radius = lens,
            center = center,
        )
        drawCircle(
            color = Color(0xFFDCE8F2),
            radius = lens,
            center = center,
            style = Stroke(width = s * 0.065f),
        )
        drawArc(
            color = Color.White.copy(alpha = 0.6f),
            startAngle = 195f,
            sweepAngle = 65f,
            useCenter = false,
            topLeft = Offset(center.x - lens * 0.62f, center.y - lens * 0.62f),
            size = Size(lens * 1.24f, lens * 1.24f),
            style = Stroke(width = s * 0.04f, cap = StrokeCap.Round),
        )
    }
}

/** The green arrow of the Downloads icon: here it means "bring an archive in". */
@Composable
fun AddApkIcon(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        val center = Offset(s / 2f, s / 2f)

        drawCircle(
            brush = Brush.radialGradient(
                colors = listOf(Color(0xFF4B545A), Color(0xFF14181A)),
                center = Offset(s * 0.34f, s * 0.28f),
                radius = s * 0.85f,
            ),
            radius = s * 0.44f,
            center = center,
        )
        drawCircle(
            color = Color(0xFF77808A).copy(alpha = 0.75f),
            radius = s * 0.44f,
            center = center,
            style = Stroke(width = s * 0.02f),
        )

        val stem = s * 0.055f
        val arrow = Path().apply {
            moveTo(center.x, center.y + s * 0.21f)
            lineTo(center.x - s * 0.17f, center.y + s * 0.02f)
            lineTo(center.x - stem, center.y + s * 0.02f)
            lineTo(center.x - stem, center.y - s * 0.20f)
            lineTo(center.x + stem, center.y - s * 0.20f)
            lineTo(center.x + stem, center.y + s * 0.02f)
            lineTo(center.x + s * 0.17f, center.y + s * 0.02f)
            close()
        }
        drawPath(
            path = arrow,
            brush = Brush.linearGradient(
                colors = listOf(Color(0xFFC4F07B), Color(0xFF4E9B22)),
                start = Offset(center.x, center.y - s * 0.2f),
                end = Offset(center.x, center.y + s * 0.21f),
            ),
        )
        drawRoundRect(
            color = Color(0xFF74B33C),
            topLeft = Offset(center.x - s * 0.18f, center.y + s * 0.27f),
            size = Size(s * 0.36f, s * 0.055f),
            cornerRadius = CornerRadius(s * 0.028f),
        )
    }
}

@Composable
fun SettingsIcon(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        val center = Offset(s / 2f, s / 2f)
        val body = Brush.linearGradient(
            colors = listOf(Color(0xFFEDF1F4), Color(0xFF98A4AE)),
            start = Offset(s * 0.15f, s * 0.10f),
            end = Offset(s * 0.85f, s * 0.90f),
        )
        val outer = s * 0.40f

        repeat(8) { index ->
            rotate(index * 45f, pivot = center) {
                drawRoundRect(
                    brush = body,
                    topLeft = Offset(center.x - s * 0.075f, center.y - outer - s * 0.03f),
                    size = Size(s * 0.15f, s * 0.30f),
                    cornerRadius = CornerRadius(s * 0.03f),
                )
            }
        }
        drawCircle(brush = body, radius = outer, center = center)
        drawCircle(
            color = Color(0xFF5A646C),
            radius = outer * 0.38f,
            center = center,
        )
        drawCircle(
            color = Color(0xFF1E2428),
            radius = outer * 0.31f,
            center = center,
        )
        drawCircle(
            color = Color.White.copy(alpha = 0.5f),
            radius = outer,
            center = center,
            style = Stroke(width = s * 0.014f),
        )
    }
}

/** Nine dots in a steel square: the app drawer of the era. */
@Composable
fun AppsIcon(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val s = size.minDimension
        drawRoundRect(
            brush = Brush.linearGradient(
                colors = listOf(Color(0xFF66747F), Color(0xFF232B31)),
                start = Offset(s * 0.1f, s * 0.1f),
                end = Offset(s * 0.9f, s * 0.9f),
            ),
            topLeft = Offset(s * 0.07f, s * 0.07f),
            size = Size(s * 0.86f, s * 0.86f),
            cornerRadius = CornerRadius(s * 0.14f),
        )
        drawRoundRect(
            color = Color.White.copy(alpha = 0.35f),
            topLeft = Offset(s * 0.07f, s * 0.07f),
            size = Size(s * 0.86f, s * 0.86f),
            cornerRadius = CornerRadius(s * 0.14f),
            style = Stroke(width = s * 0.014f),
        )
        for (row in 0 until 3) {
            for (column in 0 until 3) {
                drawCircle(
                    color = Color(0xFFEFF4F8),
                    radius = s * 0.048f,
                    center = Offset(s * (0.30f + column * 0.20f), s * (0.30f + row * 0.20f)),
                )
            }
        }
    }
}

@Composable
fun BackGlyph(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val width = size.width
        val height = size.height
        val stroke = size.minDimension * 0.11f
        drawLine(
            color = Color(0xFFE8E8E8),
            start = Offset(width * 0.74f, height * 0.20f),
            end = Offset(width * 0.30f, height * 0.50f),
            strokeWidth = stroke,
            cap = StrokeCap.Round,
        )
        drawLine(
            color = Color(0xFFE8E8E8),
            start = Offset(width * 0.30f, height * 0.50f),
            end = Offset(width * 0.74f, height * 0.80f),
            strokeWidth = stroke,
            cap = StrokeCap.Round,
        )
        drawLine(
            color = Color(0xFFE8E8E8),
            start = Offset(width * 0.30f, height * 0.50f),
            end = Offset(width * 0.80f, height * 0.50f),
            strokeWidth = stroke,
            cap = StrokeCap.Round,
        )
    }
}

@Composable
fun HomeGlyph(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val width = size.width
        val height = size.height
        val house = Path().apply {
            moveTo(width * 0.16f, height * 0.47f)
            lineTo(width * 0.50f, height * 0.20f)
            lineTo(width * 0.84f, height * 0.47f)
            moveTo(width * 0.24f, height * 0.44f)
            lineTo(width * 0.24f, height * 0.80f)
            lineTo(width * 0.76f, height * 0.80f)
            lineTo(width * 0.76f, height * 0.44f)
        }
        drawPath(
            path = house,
            color = Color(0xFFE8E8E8),
            style = Stroke(
                width = size.minDimension * 0.11f,
                cap = StrokeCap.Round,
                join = StrokeJoin.Round,
            ),
        )
    }
}

@Composable
fun RecentsGlyph(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val width = size.width
        val height = size.height
        val stroke = size.minDimension * 0.085f
        val corner = CornerRadius(size.minDimension * 0.08f)
        drawRoundRect(
            color = Color(0xFFB4B4B4),
            topLeft = Offset(width * 0.12f, height * 0.32f),
            size = Size(width * 0.56f, height * 0.40f),
            cornerRadius = corner,
            style = Stroke(width = stroke),
        )
        drawRoundRect(
            color = Color(0xFFE8E8E8),
            topLeft = Offset(width * 0.30f, height * 0.24f),
            size = Size(width * 0.58f, height * 0.44f),
            cornerRadius = corner,
            style = Stroke(width = stroke),
        )
    }
}
