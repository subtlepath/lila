"""Mexican Spanish respelling (tools/packc/respell.py, PLAN.md 4.2)."""

from __future__ import annotations

import os
import re
import tempfile
import unittest

from packc import yamlio
from packc.respell import FUNCTION_WORDS, RESPELLING_RE, Respeller

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
CONTENT_FILE = os.path.join(ROOT, "content", "pronunciation.yaml")
FIXTURE_FILE = os.path.join(ROOT, "..", "..", "test", "tinta", "fixtures", "content-mini", "pronunciation.yaml")


class RespellerCase(unittest.TestCase):
    rules = Respeller()
    full = Respeller.from_file(CONTENT_FILE)

    def check(self, cases: dict[str, str], respeller: Respeller | None = None) -> None:
        r = respeller or self.rules
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(r.text(text), expected)


class PlanExamplesTest(RespellerCase):
    """The examples PLAN.md and the content style guide rely on."""

    def test_examples(self):
        self.check({
            "chamba": "CHAHM-bah", "gracias": "GRAH-syahs", "aguacate": "ah-gwah-KAH-teh",
            "hola": "OH-lah", "México": "MEH-hee-koh", "Oaxaca": "wah-HAH-kah",
            "Xochimilco": "soh-chee-MEEL-koh", "taxi": "TAHK-see", "usted": "oos-TEHD",
            "ustedes": "oos-TEH-dehs", "buenos días": "BWEH-nohs DEE-ahs", "¿qué onda?": "KEH OHN-dah",
            "por favor": "pohr fah-BOHR", "¿mande?": "MAHN-deh", "jitomate": "hee-toh-MAH-teh",
            "cerveza": "sehr-BEH-sah", "llave": "YAH-beh", "niño": "NEE-nyoh", "perro": "PEH-rroh",
            "pero": "PEH-roh", "huevo": "WEH-boh", "hielo": "YEH-loh", "quiero": "KYEH-roh",
            "guitarra": "gee-TAH-rrah", "pingüino": "peen-GWEE-noh", "hoy": "OY", "muy": "MWEE",
            "cuate": "KWAH-teh", "bien": "BYEHN", "ciudad": "syoo-DAHD",
        }, self.full)

    def test_word_matches_text_for_single_words(self):
        for word in ("chamba", "México", "sol", "de", "¿mande?"):
            with self.subTest(word=word):
                self.assertEqual(self.full.word(word), self.full.text(word))


class RulesTest(RespellerCase):
    def test_vowels(self):
        self.check({"casa": "KAH-sah", "mesa": "MEH-sah", "libro": "LEE-broh", "tú": "TOO",
                    "uno": "OO-noh", "teléfono": "teh-LEH-foh-noh"})

    def test_rising_diphthongs(self):
        self.check({"piano": "PYAH-noh", "tiempo": "TYEHM-poh", "adiós": "ah-DYOHS",
                    "viuda": "BYOO-dah", "cuando": "KWAHN-doh", "bueno": "BWEH-noh",
                    "antiguo": "ahn-TEE-gwoh", "cuidado": "kwee-DAH-doh", "Luis": "LWEES"})

    def test_falling_diphthongs(self):
        self.check({"hay": "EYE", "aire": "EYE-reh", "baile": "BYE-leh", "veinte": "BAYN-teh",
                    "rey": "RRAY", "ley": "LAY", "estoy": "ehs-TOY", "oigo": "OY-goh", "auto": "OW-toh",
                    "aunque": "OWN-keh", "deuda": "DEH-oo-dah", "Europa": "eh-oo-ROH-pah"})

    def test_triphthongs(self):
        self.check({"Uruguay": "oo-roo-GWYE", "buey": "BWAY", "güey": "GWAY", "miau": "MYOW"})

    def test_hiatus(self):
        self.check({"día": "DEE-ah", "país": "pah-EES", "maíz": "mah-EES", "leer": "leh-EHR",
                    "oír": "oh-EER", "frío": "FREE-oh", "océano": "oh-SEH-ah-noh", "búho": "BOO-oh",
                    "ahora": "ah-OH-rah"})

    def test_seseo(self):
        self.check({"zapato": "sah-PAH-toh", "cielo": "SYEH-loh", "cena": "SEH-nah", "luz": "LOOS",
                    "lápiz": "LAH-pees", "acción": "ahk-SYOHN"})

    def test_s_sound_merges_across_syllables(self):
        self.check({"piscina": "pee-SEE-nah", "descender": "deh-sehn-DEHR",
                    "excelente": "ehk-seh-LEHN-teh", "desde": "DEHS-deh"})

    def test_yeismo_and_b_v(self):
        self.check({"calle": "KAH-yeh", "yo": "YOH", "playa": "PLAH-yah", "ayer": "ah-YEHR",
                    "vaca": "BAH-kah", "invierno": "een-BYEHR-noh", "tortilla": "tohr-TEE-yah"})

    def test_k_sounds(self):
        self.check({"queso": "KEH-soh", "aquí": "ah-KEE", "kilo": "KEE-loh", "coco": "KOH-koh",
                    "cuánto": "KWAHN-toh", "quásar": "KWAH-sahr"})

    def test_g_and_j(self):
        self.check({"gente": "HEHN-teh", "girar": "hee-RAHR", "jefe": "HEH-feh", "gato": "GAH-toh",
                    "guerra": "GEH-rrah", "bilingüe": "bee-LEEN-gweh", "agua": "AH-gwah",
                    "ignorar": "eeg-noh-RAHR"})

    def test_h(self):
        self.check({"hablar": "ah-BLAHR", "huipil": "wee-PEEL", "cacahuate": "kah-kah-WAH-teh",
                    "Chihuahua": "chee-WAH-wah", "deshielo": "dehs-YEH-loh", "ahí": "ah-EE"})

    def test_n_tilde(self):
        self.check({"año": "AH-nyoh", "señora": "seh-NYOH-rah", "español": "ehs-pah-NYOHL"})

    def test_r(self):
        self.check({"Roma": "RROH-mah", "rojo": "RROH-hoh", "honra": "OHN-rrah",
                    "alrededor": "ahl-rreh-deh-DOHR", "Israel": "ees-rrah-EHL", "caro": "KAH-roh",
                    "carro": "KAH-rroh", "tres": "TREHS", "hablar": "ah-BLAHR"})

    def test_x_by_rule(self):
        self.check({"taxi": "TAHK-see", "examen": "ehk-SAH-mehn", "éxito": "EHK-see-toh",
                    "texto": "TEHKS-toh", "explicar": "ehks-plee-KAHR", "xilófono": "see-LOH-foh-noh"})

    def test_tl(self):
        self.check({"atleta": "ah-TLEH-tah", "tlapalería": "tlah-pah-leh-REE-ah",
                    "náhuatl": "NAH-wahtl", "Mazatlán": "mah-sah-TLAHN"})

    def test_y_vowel(self):
        self.check({"y": "EE", "hoy": "OY", "muy": "MWEE", "Ana y Luis": "AH-nah ee LWEES"})

    def test_final_consonants(self):
        self.check({"usted": "oos-TEHD", "reloj": "rreh-LOH", "rock": "RROHK", "fútbol": "FOOT-bohl"})


class StressTest(RespellerCase):
    def test_monosyllable_alone_is_capitals(self):
        self.check({"sol": "SOHL", "sí": "SEE", "de": "DEH", "el": "EHL", "y": "EE", "pan": "PAHN"})
        self.assertEqual(self.rules.word("la"), "LAH")

    def test_function_words_lower_case_in_phrases(self):
        self.check({
            "de nada": "deh NAH-dah", "con permiso": "kohn pehr-MEE-soh",
            "el sol": "ehl SOHL", "mi casa": "mee KAH-sah", "a mí": "ah MEE", "tú y yo": "TOO ee YOH",
            "sé que sí": "SEH keh SEE", "La cuenta, por favor.": "lah KWEHN-tah pohr fah-BOHR",
            "no manches": "NOH MAHN-chehs", "muy bien": "MWEE BYEHN",
        })

    def test_phrase_of_function_words_still_has_a_stress(self):
        self.check({"lo que": "loh KEH", "de la": "deh LAH"})

    def test_one_stressed_syllable_per_content_word(self):
        words = ("aguacate computadora desafortunadamente electrodoméstico otorrinolaringólogo "
                 "Uruguay deuda pingüino cacahuate Popocatépetl órale cuídate").split()
        for word in words:
            with self.subTest(word=word):
                spelled = self.rules.word(word)
                stressed = [s for s in spelled.split("-") if s.isupper()]
                self.assertEqual(len(stressed), 1, spelled)

    def test_budget(self):
        for word in ("otorrinolaringólogo", "desafortunadamente", "internacionalización",
                     "electrodoméstico", "responsabilidad", "extraordinariamente"):
            with self.subTest(word=word):
                self.assertLessEqual(len(self.rules.word(word)), 40)


class FormTest(RespellerCase):
    def test_punctuation_dropped_one_space_between_words(self):
        self.check({
            "¡Órale!": "OH-rah-leh", "“Hola”, dijo.": "OH-lah DEE-hoh",
            "Sí — claro…": "SEE KLAH-roh", "¿Me trae dos tacos al pastor, por favor?":
            "meh TRAH-eh DOHS TAH-kohs ahl pahs-TOHR pohr fah-BOHR",
            "  muy   bien  ": "MWEE BYEHN",
        })
        self.assertEqual(self.rules.text("¿?"), "")

    def test_output_is_plain_ascii(self):
        texts = ("chamba", "¿Qué onda, güey?", "pingüino", "señora", "François", "Müller", "acción",
                 "hielo y huevo", "Xcaret")
        for text in texts:
            with self.subTest(text=text):
                self.assertRegex(self.full.text(text), RESPELLING_RE)

    def test_case_and_unicode_normalisation(self):
        decomposed = "México"  # e + combining acute
        self.assertEqual(self.full.text(decomposed), "MEH-hee-koh")
        self.assertEqual(self.rules.text("NIÑO"), "NEE-nyoh")

    def test_word_with_phrase_delegates_to_text(self):
        self.assertEqual(self.rules.word("buenos días"), "BWEH-nohs DEE-ahs")


class OverrideTest(RespellerCase):
    def test_override_verbatim_and_case_insensitive(self):
        r = Respeller({"México": "MEH-hee-koh", "hot dog": "hoht DOHG"})
        self.assertEqual(r.word("méxico"), "MEH-hee-koh")
        self.assertEqual(r.text("Vivo en México."), "BEE-boh ehn MEH-hee-koh")
        self.assertEqual(r.text("Un hot dog, por favor"), "oon hoht DOHG pohr fah-BOHR")
        self.assertEqual(r.word("hot dog"), "hoht DOHG")

    def test_hyphenated_token_falls_back_to_joined_key(self):
        self.assertEqual(self.full.text("Mi e-mail"), "mee EE-mayl")

    def test_bad_respelling_rejected(self):
        with self.assertRaises(ValueError):
            Respeller({"méxico": "ˈme.xi.ko"})

    def test_missing_file_means_no_overrides(self):
        r = Respeller.from_file(os.path.join(ROOT, "no-such-dir", "pronunciation.yaml"))
        self.assertEqual(r.overrides, {})
        self.assertEqual(r.word("México"), "MEHK-see-koh")

    def test_file_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pronunciation.yaml")
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("overrides:\n  no: NOH\n  Xola: SHOH-lah\n")
            r = Respeller.from_file(path)
            self.assertEqual(r.overrides, {"no": "NOH", "xola": "SHOH-lah"})
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("overrides:\n  méxico: [MEH, hee, koh]\n")
            with self.assertRaises(yamlio.YamlError):
                Respeller.from_file(path)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write("overrides: MEH-hee-koh\n")
            with self.assertRaises(yamlio.YamlError):
                Respeller.from_file(path)

    def test_fixture_file(self):
        self.assertEqual(Respeller.from_file(FIXTURE_FILE).word("México"), "MEH-hee-koh")


class ContentOverridesTest(RespellerCase):
    """content/pronunciation.yaml: the four readings of x, place names and loans."""

    def test_x_readings(self):
        self.check({
            "México": "MEH-hee-koh", "mexicano": "meh-hee-KAH-noh", "mexicana": "meh-hee-KAH-nah",
            "Oaxaca": "wah-HAH-kah", "Texas": "TEH-hahs", "Xalapa": "hah-LAH-pah",
            "Xola": "SHOH-lah", "Xcaret": "eesh-kah-REHT", "Xochimilco": "soh-chee-MEEL-koh",
            "Tlaxcala": "tlahs-KAH-lah", "Ixtapa": "ees-TAH-pah", "taxi": "TAHK-see",
        }, self.full)
        self.assertNotIn("taxi", self.full.overrides)

    def test_loans(self):
        self.check({
            "wifi": "WEE-fee", "sándwich": "SAHN-weech", "pizza": "PEET-sah", "jeans": "YEENS",
            "hot dog": "hoht DOHG", "sushi": "SOO-shee", "show": "SHOH", "internet": "een-tehr-NEHT",
            "email": "EE-mayl", "OK": "oh-KAY", "okey": "oh-KAY", "hobby": "HOH-bee",
            "fútbol": "FOOT-bohl",
        }, self.full)
        self.assertNotIn("fútbol", self.full.overrides)

    def test_every_override_is_well_formed(self):
        for key, value in self.full.overrides.items():
            with self.subTest(key=key):
                self.assertEqual(key, key.lower())
                self.assertRegex(value, RESPELLING_RE)
                self.assertTrue(re.search(r"(?:^|[- ])[A-Z]+(?:$|[- ])", value), "no stressed syllable")
                for syllable in re.split(r"[- ]", value):
                    self.assertTrue(syllable.islower() or syllable.isupper(), value)
                self.assertNotIn(key, FUNCTION_WORDS)


if __name__ == "__main__":
    unittest.main()
