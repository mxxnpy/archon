package dev.archon.app.data

import org.junit.Assert.assertEquals
import org.junit.Test

/** mDNS names are attacker-controlled: they must be cleaned before display. */
class DiscoveryNameTest {
    @Test fun dropsBidiOverridesIsolatesAndArabicLetterMark() {
        val raw = "PC‮exe.⁦x⁩؜‎‏ok"
        assertEquals("PCexe.xok", Discovery.cleanName(raw))
    }

    @Test fun dropsControlCharacters() {
        assertEquals("ab", Discovery.cleanName("a\u0000\n\u007Fb"))
    }

    @Test fun keepsAtMost64CodePointsWithoutSplittingSurrogates() {
        val emoji = "🎧"  // headphones: one code point, two chars
        val out = Discovery.cleanName(emoji.repeat(70))
        assertEquals(64, out.codePointCount(0, out.length))
        assertEquals(128, out.length)
        assertEquals(false, Character.isHighSurrogate(out.last()))
    }

    @Test fun keepsOrdinaryNames() {
        assertEquals("ESTÚDIO-PC 2", Discovery.cleanName("ESTÚDIO-PC 2"))
    }
}
