package dev.armbat

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.cos
import kotlin.math.sin

/** The stages a guest goes through before its first frame is on screen. */
enum class LaunchStage {
    PREPARE,
    DEX,
    DEX_READ,
    DEX_REWRITE,
    DEX_WRITE,
    NATIVE,
    RESOURCES,
    SURFACE,
    ACTIVITY,
    READY,
}

/**
 * The wait between a tap and the guest's first frame.
 *
 * Android 4.4 had no decorated splash screen; an application that was not ready
 * yet showed its icon and the platform's Holo spinner on a plain dark window.
 * That is all this is: no scan lines, no terminal log, no cards. The spinner is
 * the only moving part, and the line under the icon says what the worker thread
 * is doing in words a user can read.
 */
@Composable
fun LaunchScreen(app: ApkInfo?, stage: LaunchStage, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier.fillMaxSize().background(HoloBlack),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier.widthIn(max = 320.dp).padding(horizontal = 24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            if (app != null) {
                AppIcon(app, Modifier.size(72.dp))
            } else {
                Box(Modifier.size(72.dp), contentAlignment = Alignment.Center) {
                    Text(
                        "A",
                        color = HoloText,
                        fontSize = 34.sp,
                        fontFamily = FontFamily.SansSerif,
                    )
                }
            }
            Spacer(Modifier.height(18.dp))
            Text(
                app?.label ?: "Armbat",
                color = HoloText,
                fontSize = 18.sp,
                fontWeight = FontWeight.Medium,
                fontFamily = FontFamily.SansSerif,
                textAlign = TextAlign.Center,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(20.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                HoloSpinner(Modifier.size(28.dp))
                Spacer(Modifier.width(12.dp))
                Text(
                    stringResource(statusOf(stage)),
                    color = HoloDim,
                    fontSize = 13.sp,
                    fontFamily = FontFamily.SansSerif,
                )
            }
        }
    }
}

/** The 12-spoke indeterminate spinner every Holo window carried. */
@Composable
private fun HoloSpinner(modifier: Modifier = Modifier) {
    val motion = rememberInfiniteTransition(label = "spinner")
    val angle by motion.animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(tween(900, easing = LinearEasing)),
        label = "angle",
    )
    Canvas(modifier) {
        val center = Offset(size.width / 2f, size.height / 2f)
        val inner = size.minDimension * 0.22f
        val outer = size.minDimension * 0.48f
        for (index in 0 until 12) {
            val radians = Math.toRadians((index * 30f + angle).toDouble())
            val direction = Offset(cos(radians).toFloat(), sin(radians).toFloat())
            drawLine(
                color = HoloBlue.copy(alpha = 0.15f + 0.85f * (index / 11f)),
                start = center + direction * inner,
                end = center + direction * outer,
                strokeWidth = 3.dp.toPx(),
                cap = StrokeCap.Round,
            )
        }
    }
}

private fun statusOf(stage: LaunchStage): Int = when (stage) {
    LaunchStage.PREPARE, LaunchStage.DEX -> R.string.launch_preparing
    LaunchStage.DEX_READ -> R.string.launch_dex_read
    LaunchStage.DEX_REWRITE -> R.string.launch_dex_rewrite
    LaunchStage.DEX_WRITE -> R.string.launch_dex_write
    LaunchStage.NATIVE -> R.string.launch_native
    LaunchStage.RESOURCES -> R.string.launch_resources
    LaunchStage.SURFACE, LaunchStage.ACTIVITY, LaunchStage.READY -> R.string.launch_starting
}
