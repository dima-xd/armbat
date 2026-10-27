package dev.armbat

import android.app.Activity
import android.app.LocaleManager
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.LocaleList
import java.util.Locale

/**
 * The container's own language, independent of the phone's.
 *
 * Android 13 keeps a per-application locale and recreates the activity when it
 * changes, so the setting only has to be written. Android 12 has no such store,
 * so the chosen tag is kept in the application's preferences and applied by
 * wrapping the activity's base context.
 */
object AppLocale {
    private const val PREFS = "armbat"
    private const val KEY = "locale"

    fun current(context: Context): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val locales = context.getSystemService(LocaleManager::class.java)?.applicationLocales
            if (locales == null || locales.isEmpty) return ""
            return locales[0].toLanguageTag()
        }
        return preferences(context).getString(KEY, "") ?: ""
    }

    fun set(activity: Activity, tag: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val manager = activity.getSystemService(LocaleManager::class.java) ?: return
            manager.applicationLocales = when {
                tag.isEmpty() -> LocaleList.getEmptyLocaleList()
                else -> LocaleList.forLanguageTags(tag)
            }
        } else {
            preferences(activity).edit().putString(KEY, tag).apply()
            activity.recreate()
        }
    }

    fun wrap(base: Context): Context {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) return base
        val tag = base.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, "") ?: ""
        if (tag.isEmpty()) return base
        val configuration = Configuration(base.resources.configuration)
        configuration.setLocale(Locale.forLanguageTag(tag))
        return base.createConfigurationContext(configuration)
    }

    private fun preferences(context: Context) =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
}
