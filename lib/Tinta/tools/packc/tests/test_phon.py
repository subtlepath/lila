"""Written syllables, stress and accent placement (tools/packc/phon.py)."""

from __future__ import annotations

import unittest

from packc import phon


class SyllabifyTest(unittest.TestCase):
    def check(self, cases: dict[str, str]) -> None:
        for word, expected in cases.items():
            with self.subTest(word=word):
                self.assertEqual("-".join(phon.syllabify(word)), expected)

    def test_diphthongs(self):
        self.check({
            "gracias": "gra-cias", "bien": "bien", "cuate": "cua-te", "ciudad": "ciu-dad",
            "viuda": "viu-da", "ruido": "rui-do", "aire": "ai-re", "peine": "pei-ne",
            "oigo": "oi-go", "auto": "au-to", "deuda": "deu-da", "huevo": "hue-vo",
            "hielo": "hie-lo", "agua": "a-gua", "aguacate": "a-gua-ca-te", "cuídate": "cuí-da-te",
        })

    def test_hiatus(self):
        self.check({
            "día": "dí-a", "país": "pa-ís", "raúl": "ra-úl", "maíz": "ma-íz", "oír": "o-ír",
            "reír": "re-ír", "caído": "ca-í-do", "búho": "bú-ho", "leer": "le-er", "creé": "cre-é",
            "poeta": "po-e-ta", "océano": "o-cé-a-no", "héroe": "hé-ro-e", "chiita": "chi-i-ta",
            "friísimo": "fri-í-si-mo", "zoológico": "zo-o-ló-gi-co", "bahía": "ba-hí-a",
        })

    def test_triphthongs(self):
        self.check({
            "uruguay": "u-ru-guay", "paraguay": "pa-ra-guay", "buey": "buey", "güey": "güey",
            "miau": "miau", "estudiáis": "es-tu-diáis",
        })

    def test_digraphs(self):
        self.check({
            "mucho": "mu-cho", "ancho": "an-cho", "calle": "ca-lle", "conllevar": "con-lle-var",
            "perro": "pe-rro", "pero": "pe-ro", "chamba": "cham-ba",
        })

    def test_qu_gu_and_gu_diaeresis(self):
        self.check({
            "queso": "que-so", "aquí": "a-quí", "quiero": "quie-ro", "guitarra": "gui-ta-rra",
            "guerra": "gue-rra", "pingüino": "pin-güi-no", "bilingüe": "bi-lin-güe",
            "agüita": "a-güi-ta",
        })

    def test_inseparable_clusters(self):
        self.check({
            "libro": "li-bro", "hablar": "ha-blar", "otro": "o-tro", "padre": "pa-dre",
            "iglesia": "i-gle-sia", "atleta": "a-tle-ta", "tlapalería": "tla-pa-le-rí-a",
            "náhuatl": "ná-huatl", "popocatépetl": "po-po-ca-té-petl", "mazatlán": "ma-za-tlán",
        })

    def test_two_consonants(self):
        self.check({
            "carta": "car-ta", "himno": "him-no", "acción": "ac-ción", "taxi": "ta-xi",
            "texto": "tex-to", "examen": "e-xa-men", "ayer": "a-yer", "cónyuge": "cón-yu-ge",
        })

    def test_three_and_four_consonants(self):
        self.check({
            "instante": "ins-tan-te", "transporte": "trans-por-te", "obstáculo": "obs-tá-cu-lo",
            "hombre": "hom-bre", "entre": "en-tre", "conflicto": "con-flic-to",
            "extraño": "ex-tra-ño", "abstracto": "abs-trac-to", "construir": "cons-truir",
            "instrumento": "ins-tru-men-to",
        })

    def test_y(self):
        self.check({
            "hoy": "hoy", "muy": "muy", "rey": "rey", "ley": "ley", "estoy": "es-toy",
            "y": "y", "reyes": "re-yes", "playa": "pla-ya", "yo": "yo",
        })

    def test_silent_h(self):
        self.check({
            "ahora": "a-ho-ra", "deshielo": "des-hie-lo", "cacahuate": "ca-ca-hua-te", "ahí": "a-hí",
        })

    def test_no_vowel(self):
        self.assertEqual(phon.syllabify("sh"), ["sh"])


class StressTest(unittest.TestCase):
    def check(self, cases: dict[str, int]) -> None:
        for word, expected in cases.items():
            with self.subTest(word=word):
                self.assertEqual(phon.stress_index(word), expected)

    def test_written_accent(self):
        self.check({
            "café": 1, "árbol": 0, "lápiz": 0, "teléfono": 1, "inglés": 1, "camión": 1,
            "día": 0, "país": 1, "mamá": 1, "jóvenes": 0, "sí": 0, "qué": 0, "oír": 1,
        })

    def test_vowel_n_s_final_is_penultimate(self):
        self.check({
            "casa": 0, "chamba": 0, "gracias": 0, "aguacate": 2, "hablan": 0, "joven": 0,
            "examen": 1, "lunes": 0, "ustedes": 1, "ciudadano": 2, "cuate": 0, "guitarra": 1,
        })

    def test_other_final_is_final(self):
        self.check({
            "usted": 1, "ciudad": 1, "hablar": 1, "papel": 1, "feliz": 1, "reloj": 1,
            "estoy": 1, "uruguay": 2, "náhuatl": 0,
        })

    def test_monosyllables(self):
        self.check({"sol": 0, "muy": 0, "hoy": 0, "pan": 0})
        self.assertEqual(phon.default_stress("sol", 1), 0)

    def test_stress_index_accepts_syllables(self):
        self.assertEqual(phon.stress_index("teléfono", ["te", "lé", "fo", "no"]), 1)


class AccentTest(unittest.TestCase):
    def test_strip_accents(self):
        self.assertEqual(phon.strip_accents("camión"), "camion")
        self.assertEqual(phon.strip_accents("pingüino"), "pingüino")
        self.assertEqual(phon.strip_accents("señorío"), "señorio")
        self.assertTrue(phon.has_accent("árbol"))
        self.assertFalse(phon.has_accent("niño"))

    def test_place_accent(self):
        self.assertEqual(phon.place_accent(["ca", "sa"], 0), "casa")
        self.assertEqual(phon.place_accent(["ca", "fe"], 1), "café")
        self.assertEqual(phon.place_accent(["ar", "bol"], 0), "árbol")
        self.assertEqual(phon.place_accent(["mes"], 0), "mes")
        self.assertEqual(phon.place_accent(["ca", "mión"], 1), "camión")

    def test_place_accent_keeps_hiatus(self):
        self.assertEqual(phon.place_accent(["o", "ír", "lo"], 1), "oírlo")
        self.assertEqual(phon.place_accent(phon.syllabify("países"), 1), "países")

    def test_plurals_as_forms_builds_them(self):
        # forms.pluralize: stress of the singular, accent placed on the plural.
        cases = {"joven": ("joven", "jóvenes"), "camión": ("camión", "camiones"),
                 "lápiz": ("lápic", "lápices"), "inglés": ("inglés", "ingleses"),
                 "examen": ("examen", "exámenes"), "canción": ("canción", "canciones"),
                 "árbol": ("árbol", "árboles")}
        for word, (stem, plural) in cases.items():
            with self.subTest(word=word):
                stressed = phon.stress_index(word)
                plain = phon.syllabify(phon.strip_accents(stem) + "es")
                self.assertEqual(phon.place_accent(plain, stressed), plural)

    def test_attach(self):
        cases = [
            (("llama", "te"), "llámate"), (("diga", "me"), "dígame"), (("da", "me", "lo"), "dámelo"),
            (("oír", "lo"), "oírlo"), (("comiendo", "se"), "comiéndose"), (("reír", "se"), "reírse"),
            (("di", "me"), "dime"), (("haz", "lo"), "hazlo"), (("pon", "te", "lo"), "póntelo"),
            (("comer", "lo"), "comerlo"), (("cuida", "te"), "cuídate"), (("sigue", "me"), "sígueme"),
            (("averigua", "lo"), "averígualo"), (("lee", "lo"), "léelo"),
            (("construye", "lo"), "constrúyelo"), (("leyendo", "lo"), "leyéndolo"),
            (("sonríe", "me"), "sonríeme"), (("continúa", "lo"), "continúalo"),
            (("siente", "se"), "siéntese"), (("levanta", "te"), "levántate"),
            (("diga", "se", "lo"), "dígaselo"),
        ]
        for parts, expected in cases:
            with self.subTest(parts=parts):
                self.assertEqual(phon.attach(*parts), expected)


if __name__ == "__main__":
    unittest.main()
