package dev.armbat

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class UpdateRulesTest {

    @Test
    fun readsVersionCodeFromAssetName() {
        assertEquals(3, UpdateRules.versionCode("armbat-0.1.2-3.apk"))
        assertEquals(4, UpdateRules.versionCode("armbat-0.1.2-4-unsigned.apk"))
        assertNull(UpdateRules.versionCode("armbat-latest.apk"))
    }

    @Test
    fun ordersVersionsByNumbersNotText() {
        assertTrue(UpdateRules.compareVersions("0.1.10", "0.1.9") > 0)
        assertTrue(UpdateRules.compareVersions("0.2", "0.10") < 0)
        assertEquals(0, UpdateRules.compareVersions("v0.1.2", "0.1.2"))
    }

    @Test
    fun readsBuildIdFromNightlyNotes() {
        val notes = "Automatic build of 1aff7fa.\n\n" +
            "build: 1aff7fa1234567890abcdef1234567890abcdef\n" +
            "- version: 0.1.2 (3)"
        assertEquals(
            "1aff7fa1234567890abcdef1234567890abcdef",
            UpdateRules.buildId(notes),
        )
    }

    @Test
    fun hasNoBuildIdInReleaseNotes() {
        assertNull(UpdateRules.buildId("### Armbat 0.1.2 (3)\n\n- file: `armbat-0.1.2-3.apk`"))
    }

    @Test
    fun treatsAbbreviatedBuildIdAsTheSameCommit() {
        val full = "1aff7fa1234567890abcdef1234567890abcdef"
        assertTrue(UpdateRules.sameBuild(full, "1aff7fa"))
        assertTrue(UpdateRules.sameBuild("1aff7fa1234", full))
        assertFalse(UpdateRules.sameBuild(full, "2bff7fa"))
        assertFalse(UpdateRules.sameBuild("", "1aff7fa"))
    }

    @Test
    fun onlyAHigherCodeReplacesABuildOfTheSameChannel() {
        assertTrue(replaces(version = "0.1.5", code = 6, sameChannel = true))
        assertFalse(replaces(version = "0.1.4", code = 5, sameChannel = true))
        assertFalse(replaces(version = "0.1.3", code = 4, sameChannel = true))
    }

    @Test
    fun offersTheOtherChannelEvenWhenTheCodeIsTheSame() {
        assertTrue(replaces(version = "0.1.4", code = 5, sameChannel = false))
        assertFalse(replaces(version = "0.1.3", code = 4, sameChannel = false))
    }

    @Test
    fun fallsBackToTheTagWhenTheNameCarriesNoCode() {
        assertTrue(replaces(version = "0.1.5", code = null, sameChannel = true))
        assertFalse(replaces(version = "0.1.4", code = null, sameChannel = true))
        assertFalse(replaces(version = "0.1.4", code = null, sameChannel = false))
    }

    /** The installed build is always 0.1.4 (5) in these cases. */
    private fun replaces(version: String, code: Int?, sameChannel: Boolean): Boolean =
        UpdateRules.replaces(
            version = version,
            code = code,
            localCode = 5,
            localVersion = "0.1.4",
            sameChannel = sameChannel,
        )
}
