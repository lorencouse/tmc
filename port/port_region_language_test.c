/*
 * Region language-slot mapping.
 *
 * The 7-entry translation table in the ROM is shared across regions:
 *   index 0 = JP, 1 = USA English, 2..6 = the EU ROM's EN/FR/DE/ES/IT.
 * The save header stores that same index, which is why the decomp treats
 * `language >= 2` as "European" (fileselect.c's extra file-select row,
 * staffroll.c, objectA2.c). Everything below pins that mapping down, plus the
 * invariant that port_rom.c's loader populates every slot the header can
 * legally hold -- loading only 1..5 left EU Italian permanently NULL.
 */

#include <stdio.h>

#include "main.h"

int gActiveRegion = TMC_REGION_USA;

static int sFailures;

#define CHECK_EQ(actual, expected, message)                                                                      \
    do {                                                                                                         \
        int got__ = (int)(actual);                                                                               \
        int want__ = (int)(expected);                                                                            \
        if (got__ != want__) {                                                                                   \
            fprintf(stderr, "FAIL: %s: got %d expected %d\n", message, got__, want__);                           \
            sFailures++;                                                                                         \
        }                                                                                                        \
    } while (0)

int main(void) {
    gActiveRegion = TMC_REGION_USA;
    CHECK_EQ(RegionLanguageSlotCount(), NUM_LANGUAGES, "USA uses the public language slot count");
    CHECK_EQ(RegionDefaultLanguage(), LANGUAGE_EN, "USA defaults to English");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_EN), LANGUAGE_EN, "USA English preference is slot 1");
    CHECK_EQ(RegionSaveLanguageValid(LANGUAGE_EN), TRUE, "USA accepts English save slot");
    {
        u32 first = 99, last = 99;
        RegionTranslationSlotRange(&first, &last);
        CHECK_EQ(first, LANGUAGE_EN, "USA loader fills only English");
        CHECK_EQ(last, LANGUAGE_EN, "USA loader fills only English");
    }

    gActiveRegion = TMC_REGION_EU;
    CHECK_EQ(RegionLanguageSlotCount(), LANGUAGE_SLOT_COUNT, "EU exposes slot 6 for Italian resources");
    CHECK_EQ(RegionDefaultLanguage(), EU_LANGUAGE_EN_SLOT, "EU defaults to internal English slot 2");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_JP), EU_LANGUAGE_EN_SLOT,
             "EU maps unsupported Japanese preference to English");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_EN), EU_LANGUAGE_EN_SLOT,
             "EU maps user-facing English to internal slot 2");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_FR), 3, "EU maps French to internal slot 3");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_IT), 6, "EU maps Italian to internal slot 6");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(NUM_LANGUAGES), -1, "EU rejects out-of-range preference");
    CHECK_EQ(RegionSaveLanguageValid(LANGUAGE_EN), FALSE, "EU rejects USA English slot 1");
    CHECK_EQ(RegionSaveLanguageValid(EU_LANGUAGE_EN_SLOT), TRUE, "EU accepts English slot 2");
    CHECK_EQ(RegionSaveLanguageValid(EU_LANGUAGE_LAST_SLOT), TRUE, "EU accepts Italian slot 6");
    CHECK_EQ(RegionSaveLanguageValid(LANGUAGE_SLOT_COUNT), FALSE, "EU rejects slot 7");

    /* The loader-range invariant. port_rom.c fills gTranslations over exactly
     * RegionTranslationSlotRange(); pin the range itself, since that is the
     * value the loader consumes. The old hand-written `i <= 5` bound stopped
     * one short of the header's top slot and left EU Italian NULL. */
    {
        u32 first = 99, last = 99;

        RegionTranslationSlotRange(&first, &last);
        CHECK_EQ(first, EU_LANGUAGE_EN_SLOT, "EU loader starts at English slot 2");
        CHECK_EQ(last, EU_LANGUAGE_LAST_SLOT, "EU loader reaches Italian slot 6");
        CHECK_EQ(last < LANGUAGE_SLOT_COUNT, TRUE, "EU loader stays inside gTranslations");
        /* Every language the user can pick must land in a slot the loader filled. */
        for (s32 lang = 0; lang < NUM_LANGUAGES; ++lang) {
            s32 slot = RegionPreferredLanguageToSaveSlot(lang);
            if (slot >= 0) {
                CHECK_EQ((u32)slot >= first && (u32)slot <= last, TRUE,
                         "EU preferred language maps into the loaded slot range");
            }
        }
    }
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_DE), 4, "EU maps German to internal slot 4");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_ES), 5, "EU maps Spanish to internal slot 5");

    gActiveRegion = TMC_REGION_JP;
    CHECK_EQ(RegionLanguageSlotCount(), NUM_LANGUAGES, "JP uses public language slot count");
    CHECK_EQ(RegionDefaultLanguage(), LANGUAGE_JP, "JP defaults to Japanese");
    CHECK_EQ(RegionPreferredLanguageToSaveSlot(LANGUAGE_EN), LANGUAGE_JP,
             "JP maps any preference to Japanese");
    CHECK_EQ(RegionSaveLanguageValid(LANGUAGE_JP), TRUE, "JP accepts Japanese save slot");
    CHECK_EQ(RegionSaveLanguageValid(LANGUAGE_EN), FALSE, "JP rejects English save slot");
    {
        u32 first = 99, last = 99;
        RegionTranslationSlotRange(&first, &last);
        CHECK_EQ(first, LANGUAGE_JP, "JP loader fills only Japanese");
        CHECK_EQ(last, LANGUAGE_JP, "JP loader fills only Japanese");
    }

    if (sFailures != 0) {
        return 1;
    }
    printf("port_region_language_test: ALL PASS\n");
    return 0;
}
