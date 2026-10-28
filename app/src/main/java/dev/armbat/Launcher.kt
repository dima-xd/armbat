package dev.armbat

import android.graphics.BitmapFactory
import androidx.annotation.StringRes
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.Locale

internal val HoloBlack = Color(0xFF000000)
internal val HoloText = Color(0xFFF2F2F2)
internal val HoloDim = Color(0xFF9A9A9A)
internal val HoloBlue = Color(0xFF33B5E5)
internal val HoloRow = Color(0xFF1A1A1A)
internal val HoloRule = Color(0xFF2E2E2E)
internal val HoloButtonFill = Color(0xFF2B2B2B)
internal val HoloButtonRule = Color(0xFF5A5A5A)
internal val HoloRust = Color(0xFFD9694E)

private const val COLUMNS = 4
private const val ROWS = 4
private const val ITEMS_PER_PAGE = COLUMNS * ROWS

/**
 * The wallpaper of the reference launcher, rebuilt as gradients: a near-black
 * base with a cold glow along the top, teal in the lower left corner and a warm
 * red in the lower right. Drawing it keeps it sharp at any density and needs no
 * bitmap.
 */
@Composable
fun LauncherWallpaper(modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val width = size.width
        val height = size.height
        drawRect(Color(0xFF05070A))

        fun glow(x: Float, y: Float, radius: Float, color: Color, alpha: Float) {
            val center = Offset(width * x, height * y)
            drawCircle(
                brush = Brush.radialGradient(
                    colors = listOf(color.copy(alpha = alpha), Color.Transparent),
                    center = center,
                    radius = radius,
                ),
                radius = radius,
                center = center,
            )
        }

        glow(0.52f, 0.06f, width * 1.05f, Color(0xFF6C8398), 0.55f)
        glow(0.30f, 0.42f, width * 0.85f, Color(0xFF2F4A5C), 0.35f)
        glow(0.06f, 0.78f, width * 0.70f, Color(0xFF1E6E6A), 0.55f)
        glow(0.95f, 0.90f, width * 0.85f, Color(0xFF8C3546), 0.50f)
        glow(0.72f, 0.62f, width * 0.60f, Color(0xFF4A3A55), 0.30f)

        drawRect(
            Brush.verticalGradient(
                0f to HoloBlack.copy(alpha = 0.75f),
                0.18f to Color.Transparent,
                0.80f to Color.Transparent,
                1f to HoloBlack.copy(alpha = 0.65f),
            ),
        )
    }
}

/** The grey search field of the era; it filters the library as it is typed. */
@Composable
fun SearchBar(
    query: String,
    onQueryChange: (String) -> Unit,
    focusRequester: FocusRequester,
    modifier: Modifier = Modifier,
) {
    Box(
        modifier
            .fillMaxWidth()
            .height(46.dp)
            .clip(RoundedCornerShape(3.dp))
            .background(Color(0xFF8F9397))
            .border(1.dp, Color(0xFFC3C7CB), RoundedCornerShape(3.dp))
            .padding(4.dp),
    ) {
        Row(
            Modifier
                .fillMaxSize()
                .clip(RoundedCornerShape(2.dp))
                .background(Color(0xFFA9ADB1))
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SearchIcon(Modifier.size(22.dp))
            Spacer(Modifier.width(8.dp))
            Box(Modifier.weight(1f), contentAlignment = Alignment.CenterStart) {
                if (query.isEmpty()) {
                    Text(
                        stringResource(R.string.search_hint),
                        color = Color(0xFF4B4F53),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                        fontStyle = FontStyle.Italic,
                    )
                }
                BasicTextField(
                    value = query,
                    onValueChange = onQueryChange,
                    singleLine = true,
                    textStyle = TextStyle(
                        color = Color(0xFF2C3034),
                        fontSize = 16.sp,
                        fontWeight = FontWeight.Bold,
                    ),
                    cursorBrush = SolidColor(Color(0xFF2C3034)),
                    modifier = Modifier
                        .fillMaxWidth()
                        .focusRequester(focusRequester),
                )
            }
            if (query.isNotEmpty()) {
                Text(
                    "×",
                    color = Color(0xFF3A3E42),
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .clickable { onQueryChange("") }
                        .padding(horizontal = 6.dp),
                )
            }
        }
    }
}

@Composable
internal fun AppIcon(app: ApkInfo, modifier: Modifier = Modifier) {
    val icon = rememberApkIcon(app)
    Box(modifier, contentAlignment = Alignment.Center) {
        if (icon != null) {
            Image(
                bitmap = icon,
                contentDescription = null,
                contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
        } else {
            Text(
                app.label.take(1).uppercase(Locale.getDefault()),
                color = HoloText,
                fontSize = 22.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

@Composable
fun rememberApkIcon(app: ApkInfo): ImageBitmap? {
    val bitmap by produceState<ImageBitmap?>(null, app.iconPath) {
        value = withContext(Dispatchers.IO) {
            app.iconPath
                ?.let { path -> runCatching { BitmapFactory.decodeFile(path) }.getOrNull() }
                ?.asImageBitmap()
        }
    }
    return bitmap
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun LauncherTile(
    app: ApkInfo,
    loading: Boolean,
    onLaunch: () -> Unit,
    onDetails: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxSize()
            .combinedClickable(onClick = onLaunch, onLongClick = onDetails),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        AppIcon(
            app,
            Modifier
                .size(58.dp)
                .alpha(if (loading) 0.35f else 1f),
        )
        Spacer(Modifier.height(5.dp))
        Text(
            app.label,
            color = HoloText,
            fontSize = 12.sp,
            lineHeight = 14.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = TextStyle(
                shadow = Shadow(Color.Black.copy(alpha = 0.9f), Offset(0f, 2f), 4f),
            ),
        )
    }
}

@Composable
fun HomeScreen(
    apps: List<ApkInfo>,
    filtering: Boolean,
    query: String,
    onQueryChange: (String) -> Unit,
    running: String?,
    busy: Boolean,
    message: String,
    onLaunch: (ApkInfo) -> Unit,
    onDetails: (ApkInfo) -> Unit,
    onAddApk: () -> Unit,
    onSettings: () -> Unit,
    onApps: () -> Unit,
) {
    val searchFocus = remember { FocusRequester() }
    val pages = remember(apps) { apps.chunked(ITEMS_PER_PAGE) }
    val pagerState = androidx.compose.foundation.pager.rememberPagerState { pages.size }

    Box(Modifier.fillMaxSize()) {
        LauncherWallpaper(Modifier.matchParentSize())

        Column(Modifier.fillMaxSize()) {
            SearchBar(
                query = query,
                onQueryChange = onQueryChange,
                focusRequester = searchFocus,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 8.dp),
            )

            if (message.isNotEmpty()) {
                Text(
                    message,
                    color = HoloRust,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 2.dp),
                )
            }

            Box(Modifier.weight(1f)) {
                when {
                    apps.isEmpty() -> Column(
                        Modifier.fillMaxSize(),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(
                            stringResource(R.string.empty_title),
                            color = HoloText,
                            fontSize = 16.sp,
                            fontWeight = FontWeight.Bold,
                            style = TextStyle(
                                shadow = Shadow(Color.Black, Offset(0f, 2f), 4f),
                            ),
                        )
                        Spacer(Modifier.height(6.dp))
                        Text(
                            stringResource(R.string.empty_hint),
                            color = HoloDim,
                            fontSize = 13.sp,
                        )
                    }

                    filtering -> LazyVerticalGrid(
                        columns = GridCells.Fixed(COLUMNS),
                        modifier = Modifier.fillMaxSize(),
                        contentPadding = androidx.compose.foundation.layout.PaddingValues(4.dp),
                    ) {
                        items(apps, key = { it.path }) { app ->
                            Box(Modifier.height(92.dp)) {
                                LauncherTile(
                                    app = app,
                                    loading = running == app.path,
                                    onLaunch = { onLaunch(app) },
                                    onDetails = { onDetails(app) },
                                )
                            }
                        }
                    }

                    else -> HorizontalPager(
                        state = pagerState,
                        modifier = Modifier.fillMaxSize(),
                    ) { index ->
                        Column(Modifier.fillMaxSize().padding(horizontal = 4.dp)) {
                            for (row in 0 until ROWS) {
                                Row(Modifier.weight(1f).fillMaxWidth()) {
                                    for (column in 0 until COLUMNS) {
                                        val app = pages[index].getOrNull(row * COLUMNS + column)
                                        Box(Modifier.weight(1f).fillMaxHeight()) {
                                            if (app != null) {
                                                LauncherTile(
                                                    app = app,
                                                    loading = running == app.path,
                                                    onLaunch = { onLaunch(app) },
                                                    onDetails = { onDetails(app) },
                                                )
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!filtering) {
                PageDots(pagerState.currentPage, pages.size)
            }

            Dock(
                busy = busy,
                onSearch = { searchFocus.requestFocus() },
                onAddApk = onAddApk,
                onSettings = onSettings,
                onApps = onApps,
            )
        }
    }
}

@Composable
private fun PageDots(current: Int, count: Int) {
    Row(
        Modifier.fillMaxWidth().height(18.dp),
        horizontalArrangement = Arrangement.Center,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        repeat(count) { index ->
            Box(
                Modifier
                    .padding(horizontal = 4.dp)
                    .size(if (index == current) 7.dp else 6.dp)
                    .clip(CircleShape)
                    .background(if (index == current) Color.White else Color(0x66FFFFFF)),
            )
        }
    }
}

@Composable
private fun Dock(
    busy: Boolean,
    onSearch: () -> Unit,
    onAddApk: () -> Unit,
    onSettings: () -> Unit,
    onApps: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(horizontal = 10.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        DockSlot(Modifier.weight(1f), onClick = onSearch) { SearchIcon(it) }
        DockSlot(Modifier.weight(1f), onClick = onAddApk) { AddApkIcon(it.alpha(if (busy) 0.5f else 1f)) }
        DockSlot(Modifier.weight(1f), onClick = onSettings) { SettingsIcon(it) }
        DockSlot(Modifier.weight(1f), onClick = onApps) { AppsIcon(it) }
    }
}

@Composable
private fun DockSlot(
    modifier: Modifier,
    onClick: () -> Unit,
    icon: @Composable (Modifier) -> Unit,
) {
    Box(modifier.height(62.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier
                .size(56.dp)
                .clickable(onClick = onClick),
        ) {
            icon(Modifier.fillMaxSize())
        }
    }
}

/** The three soft keys of the reference: back, home, recents. */
@Composable
fun NavBar(onBack: () -> Unit, onHome: () -> Unit, onRecents: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .background(HoloBlack)
            .windowInsetsPadding(WindowInsets.navigationBars)
            .height(48.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        NavSlot(Modifier.weight(1f), onBack) { BackGlyph(Modifier.size(26.dp)) }
        NavSlot(Modifier.weight(1f), onHome) { HomeGlyph(Modifier.size(24.dp)) }
        NavSlot(Modifier.weight(1f), onRecents) { RecentsGlyph(Modifier.size(24.dp)) }
    }
}

@Composable
private fun NavSlot(
    modifier: Modifier,
    onClick: () -> Unit,
    glyph: @Composable () -> Unit,
) {
    Box(
        modifier.fillMaxHeight().clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        glyph()
    }
}

@Composable
private fun HoloDivider() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(HoloRule))
}

@Composable
private fun HoloHeader(title: String) {
    Column(Modifier.fillMaxWidth()) {
        Text(
            title,
            color = HoloText,
            fontSize = 20.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 14.dp, bottom = 10.dp),
        )
        Box(Modifier.fillMaxWidth().height(1.dp).background(HoloBlue.copy(alpha = 0.7f)))
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text,
        color = HoloBlue,
        fontSize = 11.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 1.sp,
        modifier = Modifier.padding(start = 16.dp, top = 16.dp, bottom = 6.dp),
    )
}

@Composable
private fun InfoLine(label: String, value: String) {
    Row(Modifier.fillMaxWidth().padding(vertical = 5.dp)) {
        Text(label, color = HoloDim, fontSize = 12.sp, modifier = Modifier.width(90.dp))
        Text(value, color = HoloText, fontSize = 13.sp)
    }
}

@Composable
private fun HoloButton(
    label: String,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    Box(
        modifier
            .height(44.dp)
            .border(1.dp, if (enabled) HoloButtonRule else HoloRule)
            .background(HoloButtonFill)
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            label,
            color = if (enabled) HoloText else HoloDim,
            fontSize = 13.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.sp,
        )
    }
}

@Composable
fun DetailsScreen(
    app: ApkInfo,
    launching: Boolean,
    launchError: String,
    onLaunch: () -> Unit,
    onDelete: () -> Unit,
) {
    Column(Modifier.fillMaxSize().background(HoloBlack)) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            AppIcon(app, Modifier.size(56.dp))
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    app.label,
                    color = HoloText,
                    fontSize = 19.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    "${app.packageName} · ${app.version}",
                    color = HoloDim,
                    fontSize = 11.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        Box(Modifier.fillMaxWidth().height(1.dp).background(HoloBlue.copy(alpha = 0.7f)))

        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 12.dp),
        ) {
            InfoLine(stringResource(R.string.size), sizeLabel(app.sizeBytes))
            InfoLine(stringResource(R.string.version), app.version.ifBlank { "" })
            InfoLine("ARMv7", arm32Label(app))
            InfoLine("ARM64", arm64Label(app))
            SectionLabel(stringResource(R.string.libraries))
            val libraries = app.arm32 + app.arm64
            if (libraries.isEmpty()) {
                Text(stringResource(R.string.none), color = HoloDim, fontSize = 13.sp)
            } else {
                for (name in libraries) {
                    Text(name, color = HoloText, fontSize = 12.sp, lineHeight = 18.sp)
                }
            }
        }

        if (launching || launchError.isNotEmpty()) {
            Text(
                if (launchError.isNotEmpty()) launchError else stringResource(R.string.launching),
                color = if (launchError.isNotEmpty()) HoloRust else HoloDim,
                fontSize = 12.sp,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
            )
        }
        Row(Modifier.fillMaxWidth().padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            HoloButton(
                label = stringResource(R.string.launch),
                enabled = !launching,
                modifier = Modifier.weight(1f),
                onClick = onLaunch,
            )
            HoloButton(
                label = stringResource(R.string.delete),
                enabled = true,
                modifier = Modifier.weight(1f),
                onClick = onDelete,
            )
        }
    }
}

/** Languages offered by Settings, in the order the selector shows them. */
private val LANGUAGE_TAGS = listOf("", "en", "ru", "es", "zh", "hi")

@Composable
private fun languageName(tag: String): String = when (tag) {
    "" -> stringResource(R.string.language_system)
    "en" -> "English"
    "ru" -> "Русский"
    "es" -> "Español"
    "zh" -> "中文"
    "hi" -> "हिन्दी"
    else -> tag
}

@Composable
fun SettingsScreen(
    localeTag: String,
    logLevel: Int,
    widescreen: Boolean,
    version: String,
    onLocale: (String) -> Unit,
    onLogLevel: (Int) -> Unit,
    onWidescreen: (Boolean) -> Unit,
    onAbout: () -> Unit,
) {
    var languageOpen by remember { mutableStateOf(false) }
    var logLevelOpen by remember { mutableStateOf(false) }

    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().background(HoloBlack)) {
            HoloHeader(stringResource(R.string.settings))
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState())) {
                SettingsRow(
                    title = stringResource(R.string.language),
                    summary = languageName(localeTag),
                    onClick = { languageOpen = true },
                )
                HoloDivider()
                // The log is the only diagnostic channel there is, so how much
                // of it is kept belongs next to the language, not behind a
                // developer property that needs adb.
                SettingsRow(
                    title = stringResource(R.string.log_level),
                    summary = stringResource(logLevelLabel(logLevel)),
                    onClick = { logLevelOpen = true },
                )
                HoloDivider()
                ToggleRow(
                    title = stringResource(R.string.widescreen),
                    summary = stringResource(
                        if (widescreen) R.string.widescreen_on else R.string.widescreen_off,
                    ),
                    checked = widescreen,
                    onToggle = { onWidescreen(!widescreen) },
                )
                HoloDivider()
                SettingsRow(
                    title = stringResource(R.string.about),
                    summary = "${stringResource(R.string.version_label)} $version",
                    onClick = onAbout,
                )
                HoloDivider()
            }
        }

        if (languageOpen) {
            LanguageDialog(
                localeTag = localeTag,
                onPick = {
                    languageOpen = false
                    onLocale(it)
                },
                onDismiss = { languageOpen = false },
            )
        }

        if (logLevelOpen) {
            LogLevelDialog(
                logLevel = logLevel,
                onPick = {
                    logLevelOpen = false
                    onLogLevel(it)
                },
                onDismiss = { logLevelOpen = false },
            )
        }
    }
}

/** The string a level is shown as; the log itself records the English name. */
@StringRes
private fun logLevelLabel(level: Int): Int = when (level) {
    ArmbatLog.OFF -> R.string.log_level_off
    ArmbatLog.ERROR -> R.string.log_level_error
    ArmbatLog.WARN -> R.string.log_level_warn
    ArmbatLog.DEBUG -> R.string.log_level_debug
    ArmbatLog.TRACE -> R.string.log_level_trace
    else -> R.string.log_level_info
}

/** A settings entry: title, current value underneath, chevron to say it opens. */
@Composable
private fun SettingsRow(title: String, summary: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(68.dp)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = HoloText, fontSize = 16.sp)
            Spacer(Modifier.height(2.dp))
            Text(summary, color = HoloDim, fontSize = 13.sp)
        }
        Spacer(Modifier.width(12.dp))
        Chevron()
    }
}

/**
 * A settings entry that is a value of its own rather than a menu: the whole row
 * is the switch, and the square on the right says what the value is.
 */
@Composable
private fun ToggleRow(
    title: String,
    summary: String,
    checked: Boolean,
    onToggle: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .height(68.dp)
            .clickable(onClick = onToggle)
            .padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, color = HoloText, fontSize = 16.sp)
            Spacer(Modifier.height(2.dp))
            Text(summary, color = HoloDim, fontSize = 13.sp)
        }
        Spacer(Modifier.width(12.dp))
        Box(
            Modifier
                .size(18.dp)
                .border(1.dp, if (checked) HoloBlue else HoloRule),
        ) {
            if (checked) {
                Box(Modifier.fillMaxSize().padding(4.dp).background(HoloBlue))
            }
        }
    }
}

@Composable
private fun Chevron() {
    Canvas(Modifier.size(9.dp)) {
        val width = size.width
        val height = size.height
        val stroke = width * 0.18f
        drawLine(
            color = HoloDim.copy(alpha = 0.85f),
            start = Offset(width * 0.15f, height * 0.10f),
            end = Offset(width * 0.80f, height * 0.50f),
            strokeWidth = stroke,
            cap = StrokeCap.Round,
        )
        drawLine(
            color = HoloDim.copy(alpha = 0.85f),
            start = Offset(width * 0.80f, height * 0.50f),
            end = Offset(width * 0.15f, height * 0.90f),
            strokeWidth = stroke,
            cap = StrokeCap.Round,
        )
    }
}

@Composable
private fun LanguageDialog(
    localeTag: String,
    onPick: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    ChoiceDialog(
        title = stringResource(R.string.language),
        options = LANGUAGE_TAGS.map { it to languageName(it) },
        selected = localeTag,
        onPick = onPick,
        onDismiss = onDismiss,
    )
}

@Composable
private fun LogLevelDialog(
    logLevel: Int,
    onPick: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    // Loudest first: the top of the list is the quiet setting a user picks when
    // a game is fine and the log is only being kept in case it stops being fine.
    ChoiceDialog(
        title = stringResource(R.string.log_level),
        options = listOf(
            ArmbatLog.OFF to stringResource(R.string.log_level_off),
            ArmbatLog.ERROR to stringResource(R.string.log_level_error),
            ArmbatLog.WARN to stringResource(R.string.log_level_warn),
            ArmbatLog.INFO to stringResource(R.string.log_level_info),
            ArmbatLog.DEBUG to stringResource(R.string.log_level_debug),
            ArmbatLog.TRACE to stringResource(R.string.log_level_trace),
        ),
        selected = logLevel,
        onPick = onPick,
        onDismiss = onDismiss,
    )
}

/** A list of exclusive choices in a panel, the way the platform of the era did it. */
@Composable
private fun <T> ChoiceDialog(
    title: String,
    options: List<Pair<T, String>>,
    selected: T,
    onPick: (T) -> Unit,
    onDismiss: () -> Unit,
) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.65f)),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier
                .width(300.dp)
                .background(Color(0xFF232323))
                .border(1.dp, Color(0xFF4A4A4A)),
        ) {
            Text(
                title,
                color = HoloBlue,
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 18.dp, bottom = 10.dp),
            )
            for ((value, label) in options) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                        .clickable { onPick(value) }
                        .padding(horizontal = 20.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        label,
                        color = HoloText,
                        fontSize = 15.sp,
                        modifier = Modifier.weight(1f),
                    )
                    RadioMark(selected = value == selected)
                }
            }
            HoloDivider()
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp),
                horizontalArrangement = Arrangement.End,
            ) {
                Text(
                    stringResource(R.string.cancel),
                    color = HoloBlue,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.sp,
                    modifier = Modifier
                        .clickable(onClick = onDismiss)
                        .padding(8.dp),
                )
            }
        }
    }
}

/**
 * An alert of the era: a dark panel over a dimmed screen, a blue title, the
 * message, and a single blue answer. The container never uses a floating snack
 * text for something the user has to acknowledge.
 */
@Composable
fun MessageDialog(title: String, text: String?, onDismiss: () -> Unit) {
    Box(
        Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.65f)),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier
                .width(300.dp)
                .background(Color(0xFF232323))
                .border(1.dp, Color(0xFF4A4A4A)),
        ) {
            Text(
                title,
                color = HoloBlue,
                fontSize = 17.sp,
                fontWeight = FontWeight.Bold,
                modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 18.dp, bottom = 8.dp),
            )
            if (!text.isNullOrEmpty()) {
                Text(
                    text,
                    color = Color(0xFFCCCCCC),
                    fontSize = 15.sp,
                    lineHeight = 21.sp,
                    modifier = Modifier.padding(start = 20.dp, end = 20.dp, bottom = 16.dp),
                )
            } else {
                Spacer(Modifier.height(8.dp))
            }
            HoloDivider()
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp),
                horizontalArrangement = Arrangement.End,
            ) {
                Text(
                    stringResource(R.string.ok),
                    color = HoloBlue,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 1.sp,
                    modifier = Modifier
                        .clickable(onClick = onDismiss)
                        .padding(8.dp),
                )
            }
        }
    }
}

@Composable
fun AboutScreen(appCount: Int, version: String) {
    Column(Modifier.fillMaxSize().background(HoloBlack)) {
        HoloHeader(stringResource(R.string.about))
        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Spacer(Modifier.height(34.dp))
            AppMark()
            Spacer(Modifier.height(18.dp))
            Text("Armbat", color = HoloText, fontSize = 22.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(5.dp))
            Text(
                "${stringResource(R.string.version_label)} $version",
                color = HoloDim,
                fontSize = 13.sp,
            )
            Spacer(Modifier.height(3.dp))
            Text(
                pluralStringResource(R.plurals.app_count, appCount, appCount),
                color = HoloDim,
                fontSize = 13.sp,
            )
            Spacer(Modifier.height(30.dp))
            HoloDivider()
            Spacer(Modifier.height(22.dp))
            Text(
                stringResource(R.string.about_description),
                color = HoloDim,
                fontSize = 14.sp,
                lineHeight = 21.sp,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(30.dp))
        }
    }
}

@Composable
private fun AppMark() {
    Box(
        Modifier
            .size(76.dp)
            .clip(RoundedCornerShape(14.dp))
            .background(
                Brush.linearGradient(
                    colors = listOf(Color(0xFF5A6873), Color(0xFF1B2227)),
                    start = Offset(0f, 0f),
                    end = Offset(300f, 300f),
                ),
            )
            .border(1.dp, Color(0xFF75828D), RoundedCornerShape(14.dp)),
        contentAlignment = Alignment.Center,
    ) {
        Text("A", color = HoloText, fontSize = 38.sp, fontWeight = FontWeight.Bold)
    }
}


@Composable
private fun RadioMark(selected: Boolean) {
    Box(
        Modifier
            .size(20.dp)
            .clip(CircleShape)
            .border(2.dp, if (selected) HoloBlue else Color(0xFF6A6A6A), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        if (selected) {
            Box(Modifier.size(9.dp).clip(CircleShape).background(HoloBlue))
        }
    }
}

@Composable
fun AppsScreen(
    apps: List<ApkInfo>,
    running: String?,
    onLaunch: (ApkInfo) -> Unit,
    onDetails: (ApkInfo) -> Unit,
) {
    Column(Modifier.fillMaxSize().background(HoloBlack)) {
        HoloHeader(stringResource(R.string.apps))
        LazyVerticalGrid(
            columns = GridCells.Fixed(COLUMNS),
            modifier = Modifier.fillMaxSize(),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(4.dp),
        ) {
            items(apps, key = { it.path }) { app ->
                Box(Modifier.height(92.dp)) {
                    LauncherTile(
                        app = app,
                        loading = running == app.path,
                        onLaunch = { onLaunch(app) },
                        onDetails = { onDetails(app) },
                    )
                }
            }
        }
    }
}

@Composable
private fun arm32Label(app: ApkInfo): String = when {
    app.arm32.isNotEmpty() -> app.arm32.size.toString()
    app.arm64.isNotEmpty() -> stringResource(R.string.arm32_none_arm64)
    else -> stringResource(R.string.arm32_none_java)
}

@Composable
private fun arm64Label(app: ApkInfo): String = when {
    app.arm64.isNotEmpty() -> app.arm64.size.toString()
    else -> stringResource(R.string.none)
}

@Composable
private fun sizeLabel(bytes: Long): String {
    val megabytes = bytes / 1048576.0
    return if (megabytes >= 1.0) {
        String.format(Locale.US, "%.1f %s", megabytes, stringResource(R.string.unit_mb))
    } else {
        String.format(Locale.US, "%.0f %s", bytes / 1024.0, stringResource(R.string.unit_kb))
    }
}
