"""End-to-end compiler tests on test/tinta/fixtures/content-mini, including seeded errors."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

from packc import forms as F
from packc import markup
from packc.build import compile_course
from packc.diag import Diagnostics
from packc.emit import emit
from packc.fold import fold
from packc.reader import Pack

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
FIXTURE = os.path.join(ROOT, "..", "..", "test", "tinta", "fixtures", "content-mini")
PACKC = os.path.join(ROOT, "tools", "packc")
LESSON = os.path.join("units", "01-saludos", "lesson-2.yaml")


class FixtureCase(unittest.TestCase):
    """Each test works on a private copy of the fixture."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="packc-test-")
        self.content = os.path.join(self.tmp, "content")
        shutil.copytree(FIXTURE, self.content)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def path(self, rel: str) -> str:
        return os.path.join(self.content, rel)

    def append(self, rel: str, text: str) -> None:
        with open(self.path(rel), "a", encoding="utf-8") as fh:
            fh.write(text)

    def replace(self, rel: str, old: str, new: str) -> None:
        with open(self.path(rel), encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn(old, text)
        with open(self.path(rel), "w", encoding="utf-8") as fh:
            fh.write(text.replace(old, new, 1))

    def add_sentence(self, es: str, en: str = "Test.") -> None:
        self.replace(LESSON, "sentences:\n", f'sentences:\n  - es: "{es}"\n    en: "{en}"\n')

    def compile(self):
        diag = Diagnostics()
        build = compile_course(self.content, diag)
        return build, diag

    def packc(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, PACKC, "--content", self.content, *args],
                              capture_output=True, text=True, cwd=self.tmp)


class CleanBuild(FixtureCase):
    def test_fixture_has_no_errors(self):
        _b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])

    def test_cli_check_and_build(self):
        result = self.packc("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("check passed", result.stdout)
        out = os.path.join(self.tmp, "out", "mini.pack")
        result = self.packc("--out", out, "--dump", "--review", "--review-dir", os.path.join(self.tmp, "rev"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.exists(out))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "out", "mini.dump.txt")))
        pages = os.listdir(os.path.join(self.tmp, "rev"))
        self.assertIn("index.html", pages)
        self.assertIn("u01-l01.html", pages)

    def test_release_refuses_unreviewed(self):
        result = self.packc("--release", "--out", os.path.join(self.tmp, "r.pack"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("not reviewed", result.stderr)
        self.assertIn("release refused", result.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "r.pack")))


class SeededErrors(FixtureCase):
    """PLAN.md M3: a bad character, a vosotros form and a lemma with no example fail the build."""

    def assert_fails(self, code: str) -> None:
        _b, diag = self.compile()
        self.assertIn(code, diag.codes(), [str(m) for m in diag.errors])
        result = self.packc("--check")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(f"[{code}]", result.stderr)

    def test_bad_character(self):
        self.add_sentence("Hola → Ana.")
        self.assert_fails("charset")

    def test_bad_character_in_gloss(self):
        self.replace("lexicon/core.tsv", "house; home", "house ☺")
        self.assert_fails("charset")

    def test_vosotros_form(self):
        self.add_sentence("¿Vosotros habláis español?", "Do you all speak Spanish?")
        self.assert_fails("peninsular")

    def test_vosotros_form_tag(self):
        self.add_sentence("{Habláis|hablar:pres.2p} inglés.")
        self.assert_fails("peninsular")

    def test_lemma_without_example(self):
        self.append("lexicon/core.tsv", "gato\t\tnoun\tm\tcat\tA1\t\t\tanimals\t\t\t\n")
        self.assert_fails("example")

    def test_peninsular_word_and_allow(self):
        self.append("lexicon/core.tsv", "piso\t\tnoun\tm\tfloor (of a building)\tA1\t\t\thome\t\t\t\n")
        self.add_sentence("La mesa está en el piso.")
        _b, diag = self.compile()
        self.assertIn("peninsular", diag.codes())
        self.replace(LESSON, '"La mesa está en el piso."\n', '"La mesa está en el piso."\n    allow: [piso]\n')
        _b, diag = self.compile()
        self.assertNotIn("peninsular", diag.codes(), [str(m) for m in diag.errors])

    def test_unlinked_and_ambiguous_words(self):
        self.add_sentence("Ana come pizza.")
        _b, diag = self.compile()
        self.assertTrue(any("unlinked word 'pizza'" in m.text for m in diag.errors))

    def test_form_mismatch(self):
        self.add_sentence("Luis {comes|comer:pres.3s} tacos.")
        _b, diag = self.compile()
        self.assertTrue(any(m.code == "form" and "expected 'come'" in m.text for m in diag.errors))

    def test_register_required(self):
        self.replace("lexicon/core.tsv", "cool; great\tA1\tinformal", "cool; great\tA1\t")
        self.assert_fails("register")

    def test_too_few_distractors(self):
        # A lone interjection in its own level has no same-group lemma within one level.
        self.append("lexicon/core.tsv", "híjole\t\tinterj\t\twow; oh no\tB2\tinformal\tx\t\t\t\t\n")
        self.replace(LESSON, "new: [casa,", "new: [híjole, casa,")
        self.add_sentence("¡Híjole!", "Oh no!")
        self.assert_fails("distractors")

    def test_digits_must_be_spelled_out(self):
        self.replace("phrasebook/basicos.yaml", "phrases:\n",
                     'phrases:\n  - es: "Son 30 pesos."\n    en: "It is 30 pesos."\n')
        self.assert_fails("digits")

    def test_digits_in_a_lesson_sentence(self):
        self.add_sentence("Ana come 3 tacos.")
        self.assert_fails("digits")

    def test_broken_data_files_are_diagnostics(self):
        self.append("pronunciation.yaml", "  : [\n")
        self.assert_fails("yaml")

    def test_broken_irregular_file_is_a_diagnostic(self):
        self.append("verbs/irregular.yaml", "comer: {colour: red}\n")
        self.assert_fails("verb")

    def test_undeclared_stem_changing_verb(self):
        self.append("lexicon/core.tsv", "pensar\t\tverb\t\tto think\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Pensar es bueno.", "Thinking is good.")
        self.assert_fails("verb")
        self.append("verbs/irregular.yaml", "pensar: {stem: e>ie}\n")
        _b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])

    def test_prefixed_verb_needs_an_entry(self):
        self.append("lexicon/core.tsv", "devolver\t\tverb\t\tto give back\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Devolver es bueno.", "Giving back is good.")
        _b, diag = self.compile()
        self.assertTrue(any(m.code == "verb" and "like volver" in m.text for m in diag.errors),
                        [str(m) for m in diag.errors])

    def test_regular_verb_with_a_listed_ending_is_fine(self):
        # presentar ends in "sentar" (e>ie) but is regular: only whole families are endings.
        self.append("lexicon/core.tsv", "presentar\t\tverb\t\tto introduce\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Presentar es bueno.", "Introducing is good.")
        _b, diag = self.compile()
        self.assertNotIn("verb", diag.codes(), [str(m) for m in diag.errors])

    def test_regular_true_silences_the_verb_guard(self):
        self.append("lexicon/core.tsv", "pensar\t\tverb\t\tto think\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Pensar es bueno.", "Thinking is good.")
        self.append("verbs/irregular.yaml", "pensar: {regular: true}\n")
        _b, diag = self.compile()
        self.assertNotIn("verb", diag.codes())

    def test_unknown_field(self):
        self.replace(LESSON, "level: A1\n", "level: A1\ncolour: red\n")
        self.assert_fails("schema")


class ParallelAuthoring(FixtureCase):
    """Per-unit fragment files and --allow-missing-examples (docs/content-style.md)."""

    def write(self, rel: str, text: str) -> None:
        os.makedirs(os.path.dirname(self.path(rel)), exist_ok=True)
        with open(self.path(rel), "w", encoding="utf-8") as fh:
            fh.write(text)

    def test_allow_missing_examples(self):
        self.append("lexicon/core.tsv", "gato\t\tnoun\tm\tcat\tA1\t\t\tanimals\t\t\t\n")
        self.assertEqual(self.packc("--check").returncode, 1)
        result = self.packc("--check", "--allow-missing-examples")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("warning [example]", result.stderr)
        result = self.packc("--release", "--allow-missing-examples", "--out", os.path.join(self.tmp, "r.pack"))
        self.assertEqual(result.returncode, 2)
        _b, diag = self.compile()
        self.assertIn("example", diag.codes())

    def test_fragments_are_merged(self):
        self.write("confusables/u05.yaml", "- name: casa / mesa\n  lemmas: [casa, mesa]\n  note: Test.\n")
        self.write("verbs/irregular/u05.yaml", "pensar: {stem: e>ie}\n")
        self.write("pronunciation/u05.yaml", "overrides:\n  ana: AH-nah-test\n")
        self.append("lexicon/core.tsv", "pensar\t\tverb\t\tto think\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Pensar es bueno.", "Thinking is good.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        self.assertIn("casa / mesa", [c["name"] for c in b.confusables])
        self.assertEqual(b.by_key["pensar"].verb.forms[("pres", "1s")], "pienso")
        self.assertEqual(b.by_key["Ana"].pron, "AH-nah-test")

    def test_a_key_defined_twice_names_both_files(self):
        self.write("confusables/u05.yaml", "- name: ser / estar\n  lemmas: [ser, estar]\n")
        self.write("verbs/irregular/u05.yaml", "ser: {stem: e>ie}\n")
        self.write("pronunciation/u05.yaml", "overrides:\n  méxico: MEH-shee-koh\n")
        _b, diag = self.compile()
        dupes = [m for m in diag.errors if m.code == "duplicate"]
        self.assertEqual(len(dupes), 3, [str(m) for m in diag.errors])
        for m in dupes:
            self.assertIn("u05.yaml", str(m.where))
        texts = " ".join(m.text for m in dupes)
        for shared in ("confusables.yaml", "irregular.yaml", "pronunciation.yaml"):
            self.assertIn(shared, texts)


class WriterRequests(FixtureCase):
    """Requests from the Unit 2-12 lesson writers."""

    def test_each_given_form_is_emitted(self):
        self.append("lexicon/core.tsv", "cuántos\t\tdet\t\thow many\tA1\t\t\tquestions\tfpl=cuántas\t\t\n")
        self.add_sentence("¿Cuántas casas?", "How many houses?")
        self.add_sentence("¿Cuántos tacos?", "How many tacos?")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        sent = next(s for s in b.sentences if s.es == "¿Cuántas casas?")
        self.assertEqual((sent.tokens[0].lemma.key, F.tag_name(sent.tokens[0].tag)), ("cuántos", "fpl"))

    def test_note_allow(self):
        note = ('notes:\n  - kind: usage\n    title: Floors\n    text: "El _piso_ is the floor."\n'
                '    allow: [piso]\n')
        self.replace(LESSON, "notes:\n", note)
        _b, diag = self.compile()
        self.assertNotIn("peninsular", diag.codes(), [str(m) for m in diag.errors])
        self.replace(LESSON, "    allow: [piso]\n", "")
        _b, diag = self.compile()
        self.assertIn("peninsular", diag.codes())

    def test_adjective_options_agree_with_the_noun(self):
        self.append("lexicon/core.tsv", "joven\t\tadj\t\tyoung\tA1\t\t\tpeople\t\t\t\n")
        self.add_sentence("La señora {joven} come.", "The young lady eats.")
        b, _ = self.compile()
        item = next(i for i in b.items if i.key == "cloze:la senora joven come:joven")
        self.assertIn("chida", item.candidates)        # feminine, like señora
        self.assertNotIn("chido", item.candidates)
        self.assertNotIn("frío", item.candidates)

    def test_review_dir_follows_the_content(self):
        result = self.packc("--check", "--review", "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "build", "review-content", "index.html")))
        out = os.path.join(self.tmp, "mine", "x.pack")
        self.assertEqual(self.packc("--out", out, "--review", "--quiet").returncode, 0)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "mine", "review", "index.html")))

    def test_reviewer_notes(self):
        b, _ = self.compile()
        before = {s.name: s.hash for s in b.review_status}
        self.replace(LESSON, "level: A1\n", 'level: A1\nreviewer_notes: ["Is taquería the usual word?"]\n')
        self.replace(LESSON, '    en: "Ana lives in a big house."\n',
                     '    en: "Ana lives in a big house."\n    reviewer_notes: "Natural in Mexico?"\n')
        result = self.packc("--check", "--review", "--review-dir", os.path.join(self.tmp, "rev"), "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(os.path.join(self.tmp, "rev", "u01-l02.html"), encoding="utf-8") as fh:
            page = fh.read()
        self.assertIn("Questions for the reviewer", page)
        self.assertIn("Is taquería the usual word?", page)
        self.assertIn("Natural in Mexico?", page)
        b, _ = self.compile()
        after = {s.name: s.hash for s in b.review_status}
        self.assertEqual(before, after)  # notes are not part of the review hash
        data, _report = emit(b)
        self.assertNotIn(b"Natural in Mexico", data)


class WriterRequestsUnits8To9(FixtureCase):
    def test_pronoun_before_an_auxiliary(self):
        rows = ("quedarse\t\tverb\t\tto stay\tA1\t\t\tcity\t\t\t\n"
                "quedar\t\tverb\t\tto be (located); to fit\tA1\t\t\tcity\t\t\t\n"
                "ir\t\tverb\t\tto go\tA1\t\t\tcity\t\t\t\n"
                "irse\t\tverb\t\tto leave\tA1\t\t\tcity\t\t\t\n"
                "a\t\tprep\t\tto; at\tA1\t\t\tgrammar\t\t\t\n"
                "se\t\tpron\t\toneself; himself; herself\tA1\t\t\tgrammar\t\t\t\n")
        self.append("lexicon/core.tsv", rows)
        self.append("verbs/irregular.yaml", "ir:\n  forms: {pres.1s: voy, pres.2s: vas, pres.3s: va, "
                    "pres.1p: vamos, pres.3p: van}\n  gerund: yendo\n")
        sentences = {
            "Me voy a quedar.": [("voy", "ir"), ("quedar", "quedarse")],
            "Voy a quedar con Ana.": [("Voy", "ir"), ("quedar", "quedar")],
            "Me estoy quedando.": [("quedando", "quedarse")],
            "Me voy.": [("voy", "irse")],
            "Ana se va a {quedar|quedarse}.": [("quedar", "quedarse")],
        }
        for es in sentences:
            self.add_sentence(es)
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        links = {s.es: [(t.surface, t.lemma.key if t.lemma else None) for t in s.tokens] for s in b.sentences}
        plain = {es.replace("{quedar|quedarse}", "quedar"): want for es, want in sentences.items()}
        for es, wanted in plain.items():
            for pair in wanted:
                self.assertIn(pair, links[es], es)

    def test_confusables_across_parts_of_speech(self):
        self.append("lexicon/core.tsv", "calor\t\tnoun\tm\theat\tA1\t\t\tweather\t\t\t\n"
                    "caliente\t\tadj\t\thot (to the touch)\tA1\t\t\tweather\t\t\t\n")
        self.append("confusables.yaml", "- name: calor / caliente\n  lemmas: [calor, caliente]\n")
        self.add_sentence("El {calor} es muy grande.", "The heat is very great.")
        self.add_sentence("El agua está {caliente}.", "The water is hot.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        cloze = by_key["cloze:el calor es muy grande:calor"]
        self.assertEqual((cloze.candidates[0], cloze.must_show), ("caliente", 1))
        self.assertEqual(by_key["cloze:el agua esta caliente:caliente"].candidates[0], "calor")

    def test_confusable_partner_always_offered(self):
        self.append("lexicon/core.tsv", "papá\t\tnoun\tm\tdad\tA1\t\t\tpeople\t\t\t\n")
        self.append("confusables.yaml", "- name: papa / papá\n  lemmas: [papa, papá]\n")
        self.add_sentence("Tacos de {papa}, por favor.", "Potato tacos, please.")
        self.add_sentence("Mi {papá} come [papas|papa].", "My dad eats potatoes.")
        self.add_sentence("Mi papá come {papas|papa}.", "My dad eats potatoes.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        for key, partner in (("cloze:tacos de papa por favor:papa", "papá"),
                             ("cloze:mi papa come papas:papa", "papa"),
                             ("cloze:mi papa come papas:papas", "papás")):
            item = by_key[key]
            self.assertEqual((item.candidates[0], item.must_show), (partner, 1), key)

    def test_both_gender_noun_follows_its_article(self):
        self.append("lexicon/core.tsv", "policía\t\tnoun\tmf\tpolice; police officer\tA1\t\t\tcity\t\t\t\n")
        self.add_sentence("Ana habla con la {policía}.", "Ana talks to the police.")
        b, _ = self.compile()
        item = next(i for i in b.items if i.key == "cloze:ana habla con la policia:policia")
        feminine = {lem.es for lem in b.lemmas if lem.pos == 1 and lem.gender == 2 and not lem.flags & 4} | {
            lem.feminine or lem.es for lem in b.lemmas if lem.pos == 1 and lem.gender == 3}
        self.assertTrue(set(item.candidates) <= feminine, item.candidates)
        self.assertNotIn("taco", item.candidates)

    def test_adjective_keeps_its_own_number_and_follows_the_subject(self):
        self.add_sentence("Mi amigo come dos tacos y está muy {frío}.", "My friend eats two tacos and is very cold.")
        self.add_sentence("La casa de mi amigo es {grande}.", "My friend's house is big.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        frio = by_key["cloze:mi amigo come dos tacos y esta muy frio:frio"].candidates
        self.assertIn("chido", frio)                     # singular, as the target shows
        self.assertFalse({"chidos", "buenos"} & set(frio), frio)
        grande = by_key["cloze:la casa de mi amigo es grande:grande"].candidates
        self.assertIn("chida", grande)                   # agrees with la casa, the subject
        self.assertNotIn("chido", grande)

    def test_adjective_after_ser_agrees_with_the_subject(self):
        self.append("lexicon/core.tsv", "increíble\t\tadj\t\tincredible\tA1\t\t\tfeelings\t\t\t\n")
        self.add_sentence("Las casas son {increíbles}.", "The houses are incredible.")
        self.append("lexicon/core.tsv", "las\t\tdet\tfpl\tthe (feminine plural)\tA1\t\t\tgrammar\t\t\t\n")
        b, _ = self.compile()
        item = next(i for i in b.items if i.key == "cloze:las casas son increibles:increibles")
        self.assertIn("chidas", item.candidates)
        self.assertNotIn("chidos", item.candidates)


class WriterRequestsUnits4To7(FixtureCase):
    def test_marked_reflexive_infinitive_options_are_bare(self):
        self.append("lexicon/core.tsv", "quedarse\t\tverb\t\tto stay\tA1\t\t\tcity\t\t\t\n"
                    "ir\t\tverb\t\tto go\tA1\t\t\tcity\t\t\t\n"
                    "irse\t\tverb\t\tto leave\tA1\t\t\tcity\t\t\t\n"
                    "a\t\tprep\t\tto; at\tA1\t\t\tgrammar\t\t\t\n")
        self.append("verbs/irregular.yaml", "ir:\n  forms: {pres.1s: voy, pres.2s: vas, pres.3s: va, "
                    "pres.1p: vamos, pres.3p: van}\n  gerund: yendo\n")
        self.add_sentence("Me voy a {quedar|quedarse}.", "I'm going to stay.")
        self.add_sentence("Me voy.", "I'm leaving.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        links = {s.es: [(t.surface, t.lemma.key) for t in s.tokens if t.lemma] for s in b.sentences}
        self.assertIn(("voy", "ir"), links["Me voy a quedar."])      # the pronoun is quedarse's
        self.assertIn(("voy", "irse"), links["Me voy."])
        item = next(i for i in b.items if i.key == "cloze:me voy a quedar:quedar")
        self.assertIn("llamar", item.candidates)                       # bare, like the gap
        self.assertFalse(any(c.endswith("se") for c in item.candidates), item.candidates)

    def test_the_other_you_form_only_when_the_sentence_decides(self):
        self.add_sentence("¿De dónde {eres}?", "Where are you from?")
        b, _ = self.compile()
        by_key = {i.key: i for i in b.items}
        self.assertNotIn("es", by_key["cloze:de donde eres:eres"].candidates)       # usted fits too
        self.assertIn("es", by_key["cloze:tu eres mi amigo:eres"].candidates)        # tú decides
        self.assertIn("eres", by_key["cloze:usted es de aqui:es"].candidates)        # usted decides
        self.assertIn("llama", by_key["cloze:como te llamas me llamo luis:llamas"].candidates)  # te

    def test_you_traps(self):
        sentences = {
            "¡{Come}!": ("Eat!", {"coma", "coman"}, set()),                        # may be a tú command
            "¿{Comes} tacos?": ("Do you eat tacos?", {"come", "comen"}, set()),     # usted and ustedes fit
            "Tú {comes} tacos.": ("You eat tacos.", set(), {"come", "comen"}),       # tú decides both
            "No {comas|comer:impneg.2s} tacos.": ("Don't eat tacos.", {"coma", "coman"}, set()),
            "Ana, ¿{comes} tacos?": ("Ana, do you eat tacos?", {"come"}, {"comen"}),  # singular vocative
        }
        for es, (en, _out, _in) in sentences.items():
            self.add_sentence(es, en)
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_sentence = {b.sentences[i.a].es: i for i in b.items if i.kind == 2}
        for es, (_en, excluded, included) in sentences.items():
            plain = es.replace("{", "").replace("}", "").replace("|comer:impneg.2s", "")
            options = {c.lower() for c in by_sentence[plain].candidates}
            self.assertFalse(excluded & options, (es, options))
            self.assertTrue(included <= options, (es, options))

    def test_address_formulas_are_you_cues(self):
        self.append("lexicon/core.tsv", "disculpe\t\tinterj\t\texcuse me (formal)\tA1\t\t\tgreetings\t\t\t\n"
                    "oye\t\tinterj\t\they; listen (informal)\tA1\t\t\tgreetings\t\t\t\n")
        self.add_sentence("Disculpe, ¿{come|comer:pres.3s} tacos?", "Excuse me, do you eat tacos?")
        self.add_sentence("Oye, ¿{comes} tacos?", "Hey, do you eat tacos?")
        self.add_sentence("¡{Come}!", "Eat!")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_sentence = {b.sentences[i.a].es: {c.lower() for c in i.candidates} for i in b.items if i.kind == 2}
        self.assertTrue({"comes", "comen"} <= by_sentence["Disculpe, ¿come tacos?"])   # usted decided
        self.assertTrue({"come", "comen"} <= by_sentence["Oye, ¿comes tacos?"])        # tú decided
        self.assertFalse({"coma", "coman"} & by_sentence["¡Come!"])

    def test_no_plural(self):
        self.append("lexicon/core.tsv", "fútbol\t\tnoun\tm\tsoccer\tA1\t\t\tfood\tpl=-\t\t\n"
                    "enero\t\tnoun\tm\tJanuary\tA1\t\t\tfood\t\t\t\n")
        self.add_sentence("El fútbol es bueno en enero.", "Soccer is good in January.")
        b, _ = self.compile()
        for key in ("fútbol", "enero"):
            self.assertEqual(b.by_key[key].plural, "", key)
            self.assertFalse(any(t == F.NOMINAL["pl"] for _f, t in b.by_key[key].forms), key)
        tacos = next(i for i in b.items if i.key == "cloze:luis come dos tacos:tacos")
        self.assertFalse({"fútboles", "eneros"} & set(tacos.candidates), tacos.candidates)

    def test_determiner_options_are_determiners_of_the_same_kind(self):
        self.append("lexicon/core.tsv",
                    "nuestro\t\tdet\t\tour\tA1\t\t\tpeople\tf=nuestra; mpl=nuestros; fpl=nuestras\t\t\n"
                    "tu\t\tdet\t\tyour (informal)\tA1\t\t\tpeople\tpl=tus\t\t\n"
                    "este\t\tdet\t\tthis\tA1\t\t\tgrammar\tf=esta; mpl=estos; fpl=estas\t\t\n"
                    "ese\t\tdet\t\tthat\tA1\t\t\tgrammar\tf=esa; mpl=esos; fpl=esas\t\t\n")
        self.add_sentence("{Nuestra} casa es grande.", "Our house is big.")
        self.add_sentence("{Este} taco es bueno.", "This taco is good.")
        self.add_sentence("Ese taco es de tu casa.", "That taco is from your house.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        possessive = by_key["cloze:nuestra casa es grande:nuestra"].candidates
        self.assertTrue(set(possessive) <= {"Mi", "Su", "Tu"} and len(possessive) >= 2, possessive)
        self.assertFalse({"Su", "Tu"} <= set(possessive))  # both mean "your": never together
        self.assertFalse({"Yo", "Tú", "Él", "Ella", "Usted"} & set(possessive))
        # A closed class may have fewer than three: este against ese only, and no error.
        self.assertEqual(by_key["cloze:este taco es bueno:este"].candidates, ["Ese"])

    def test_demonstratives(self):
        self.append("lexicon/core.tsv",
                    "este\t\tdet\t\tthis\tA1\t\t\tgrammar\tf=esta; mpl=estos; fpl=estas\t\t\n"
                    "ese\t\tdet\t\tthat\tA1\t\t\tgrammar\tf=esa; mpl=esos; fpl=esas\t\t\n"
                    "aquel\t\tdet\t\tthat (over there)\tA1\t\t\tgrammar\tf=aquella; mpl=aquellos; fpl=aquellas\t\t\n")
        self.add_sentence("{Este} taco es bueno.", "This taco is good.")
        self.add_sentence("{Esa} casa es grande.", "That house is big.")
        self.add_sentence("{Aquel} taco es bueno.", "That taco over there is good.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        self.assertEqual(set(by_key["cloze:este taco es bueno:este"].candidates), {"Ese", "Aquel"})
        self.assertEqual(by_key["cloze:esa casa es grande:esa"].candidates, ["Esta"])       # not aquella
        self.assertEqual(by_key["cloze:aquel taco es bueno:aquel"].candidates, ["Este"])    # not ese

    def test_options_are_distinct_unrelated_and_met_first(self):
        rows = ("trabajo\t\tnoun\tm\tjob; work\tA1\t\t\twork\t\t\t\n"
                "empleo\t\tnoun\tm\tjob; employment\tA1\t\t\twork\t\t\t\n"
                "oficio\t\tnoun\tm\ttrade\tA1\t\t\twork\t\t\toficios\n")
        self.append("lexicon/core.tsv", rows)
        self.add_sentence("El trabajo y el empleo son buenos.", "Work and employment are good.")
        self.add_sentence("El oficio es bueno.", "The trade is good.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        for item in b.items:
            if item.kind in (0, 1):
                lemmas = [b.lemmas[c] for c in item.candidates]
                shown = [lem.en if item.kind == 0 else lem.es for lem in lemmas]
                self.assertEqual(len(shown), len(set(s.lower() for s in shown)), item.key)
                for i, x in enumerate(lemmas):
                    for y in lemmas[i + 1:]:
                        self.assertFalse(x.senses & y.senses, (item.key, x.key, y.key))
        # Lesson 1 vocabulary prefers words met by lesson 1 over lesson 2's new words.
        casa = b.by_key["casa"]
        hola = next(i for i in b.items if i.key == "vocab:hola:recognise")
        self.assertNotIn(casa.id, hola.candidates)

    def test_untargeted_lemma_reflexive_target(self):
        self.append("lexicon/core.tsv", "quedarse\t\tverb\t\tto stay\tA1\t\t\tcity\t\t\t\n"
                    "quedar\t\tverb\t\tto be (located); to fit\tA1\t\t\tcity\t\t\t\n"
                    "ir\t\tverb\t\tto go\tA1\t\t\tcity\t\t\t\n"
                    "irse\t\tverb\t\tto leave\tA1\t\t\tcity\t\t\t\n"
                    "a\t\tprep\t\tto; at\tA1\t\t\tgrammar\t\t\t\n")
        self.append("verbs/irregular.yaml", "ir:\n  forms: {pres.1s: voy, pres.2s: vas, pres.3s: va, "
                    "pres.1p: vamos, pres.3p: van}\n  gerund: yendo\n")
        self.add_sentence("Me voy a {quedar}.", "I'm going to stay.")
        self.add_sentence("Voy a quedar con Ana.", "I'm going to meet Ana.")
        self.add_sentence("Me voy.", "I'm leaving.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        sent = next(s for s in b.sentences if s.es == "Me voy a quedar.")
        self.assertEqual([(t.surface, t.lemma.key) for t in sent.tokens][1:],
                         [("voy", "ir"), ("a", "a"), ("quedar", "quedarse")])
        item = next(i for i in b.items if i.key == "cloze:me voy a quedar:quedar")
        self.assertFalse(any(c.endswith("se") for c in item.candidates), item.candidates)


class DeckDictionaryPhrasebook(FixtureCase):
    def write(self, rel: str, text: str) -> None:
        os.makedirs(os.path.dirname(self.path(rel)), exist_ok=True)
        with open(self.path(rel), "w", encoding="utf-8") as fh:
            fh.write(text)

    def test_frequency_deck_files_and_deck_sentences(self):
        self.write("lexicon/frequency-food.tsv", "lemma\tpos\tgender\tgloss\tlevel\tfreq\ttopic\n"
                   "manzana\tnoun\tf\tapple\tA1\t30\tfood\n")
        self.write("deck/comida.yaml", 'title: Comida\nsentences:\n'
                   '  - es: "La {manzana} es buena."\n    en: "The apple is good."\n'
                   '    reviewer_notes: "Common in Mexico?"\n')
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i for i in b.items}
        rec = by_key["vocab:manzana:recognise"]
        cloze = by_key["cloze:la manzana es buena:manzana"]
        self.assertEqual((rec.lesson, cloze.lesson), (NONE, NONE))
        self.assertTrue(cloze.flags & 4)                       # frequency deck
        self.assertEqual(cloze.prereq, rec.index)
        self.assertGreater(cloze.index, by_key["vocab:manzana:produce"].index)
        self.assertGreater(rec.index, max(i.index for i in b.items if i.lesson != NONE))
        self.assertEqual(b.sentences[cloze.a].source, 4)
        status = {s.name: s for s in b.review_status}["comida"]
        self.assertEqual(status.kind, "deck")
        self.replace("lexicon/frequency-food.tsv", "apple", "apple (fruit)")
        b2, _ = self.compile()
        self.assertNotEqual({s.name: s for s in b2.review_status}["comida"].hash, status.hash)
        result = self.packc("--check", "--review", "--review-dir", os.path.join(self.tmp, "rev"), "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "rev", "deck-comida.html")))

    def test_dictionary_rows(self):
        self.write("lexicon/dictionary.tsv", "lemma\tpos\tgender\tgloss\tlevel\n"
                   "abeja\tnoun\tf\tbee\t\n"
                   "nadar\tverb\t\tto swim\tA2\n"
                   "pensar\tverb\t\tto think\t\n")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])       # no examples needed, no level needed
        notes = [m.text for m in diag.messages if m.level == "note"]
        self.assertTrue(any("pensar" in n for n in notes), notes)  # undeclared irregular: no table yet
        self.assertIsNone(b.by_key["pensar"].verb)
        self.assertIsNotNone(b.by_key["nadar"].verb)
        dictionary = {b.by_key[k].id for k in ("abeja", "nadar", "pensar")}
        for item in b.items:
            if item.kind in (0, 1, 4):
                self.assertNotIn(item.a, dictionary, item.key)           # no items of their own
            if item.kind in (0, 1):
                self.assertFalse(dictionary & set(item.candidates), item.key)  # never an option
        data, _ = emit(b)
        pack = Pack(data)
        keys = {pack.str(k["key"]) for k in pack.records("LKEY")}
        forms = {pack.str(f["key"]) for f in pack.records("FORM")}
        self.assertIn("abeja", keys)
        self.assertIn("nado", forms)
        lemma = pack.record("LEMM", b.by_key["abeja"].id)
        self.assertEqual((lemma["level"], bool(lemma["flags"] & 64)), (0, True))
        # Sentences link to dictionary rows only where marked.
        self.add_sentence("Ana nada en el agua.", "Ana swims in the water.")
        _b, diag = self.compile()
        self.assertTrue(any("unlinked word 'nada'" in m.text for m in diag.errors))
        self.replace(LESSON, '"Ana nada en el agua."', '"Ana [nada|nadar] en el agua."')
        _b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])

    def test_deck_options_vary_between_items(self):
        rows = "lemma\tpos\tgloss\tlevel\tfreq\n" + "".join(
            f"{w}\tadj\t{g}\tA2\t{n}\n" for n, (w, g) in enumerate(
                [("alto", "tall"), ("bajo", "short"), ("caro", "expensive"), ("barato", "cheap"),
                 ("feo", "ugly"), ("bonito", "pretty"), ("nuevo", "new"), ("viejo", "old"),
                 ("limpio", "clean"), ("sucio", "dirty"), ("rápido", "fast"), ("lento", "slow")], 1))
        self.write("lexicon/frequency-adj.tsv", rows)
        sentences = "".join(f'  - es: "El taco es {{{w}}}."\n    en: "The taco is {w}."\n'
                            for w in ("alto", "bajo", "caro", "barato", "feo", "bonito", "nuevo", "viejo",
                                      "limpio", "sucio", "rápido", "lento"))
        self.write("deck/adjetivos.yaml", "sentences:\n" + sentences)
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        lists = [tuple(i.candidates) for i in b.items if i.kind == 2 and i.flags & 4]
        self.assertEqual(len(lists), 12)
        self.assertEqual(len(set(lists)), len(lists))            # no two deck clozes alike
        by_key = {i.key: i.candidates for i in b.items}
        self.assertNotEqual(by_key["cloze:el taco es alto:alto"][:3], by_key["cloze:el taco es caro:caro"][:3])

    def test_verb_options_only_in_taught_tenses(self):
        self.write("deck/verbos.yaml", 'sentences:\n  - es: "Ana {come} tacos."\n    en: "Ana eats tacos."\n')
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        by_key = {i.key: i.candidates for i in b.items}
        # Unit 1 has taught only the present.
        self.assertEqual(set(by_key["conj:ser:pres.1s"]), {"eres", "es", "somos", "son"})
        # Deck items come after every lesson: preterite and imperfect, never future or conditional.
        deck = set(by_key["cloze:ana come tacos:come"])
        self.assertIn("comió", deck)
        self.assertFalse({"comerá", "comería"} & deck, deck)

    def test_dictionary_twins_never_relink_course_words(self):
        rows = ("ir\t\tverb\t\tto go\tA1\t\t\tcity\t\t\t\n"
                "a\t\tprep\t\tto; at\tA1\t\t\tgrammar\t\t\t\n"
                "quedarse\t\tverb\t\tto stay\tA1\t\t\tcity\t\t\t\n")
        self.append("lexicon/core.tsv", rows)
        self.append("verbs/irregular.yaml", "ir:\n  forms: {pres.1s: voy, pres.2s: vas, pres.3s: va, "
                    "pres.1p: vamos, pres.3p: van}\n  gerund: yendo\n")
        self.add_sentence("Me voy a hablar con Ana.", "I'm going to talk to Ana.")
        self.add_sentence("Voy a quedar con Ana.", "I'm going to stay with Ana.")
        before, _ = self.compile()
        self.write("lexicon/dictionary.tsv", "lemma\tpos\tgloss\tlevel\n"
                   "hablarse\tverb\tto talk to each other\t\n"
                   "quedar\tverb\tto arrange to meet\t\n")
        after, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        for b in (before, after):
            links = {s.es: [(t.surface, t.lemma.key) for t in s.tokens if t.lemma] for s in b.sentences}
            self.assertIn(("hablar", "hablar"), links["Me voy a hablar con Ana."])     # not hablarse
            self.assertIn(("quedar", "quedarse"), links["Voy a quedar con Ana."])       # not dictionary quedar

    def test_dictionary_may_define_peninsular_words_with_a_note(self):
        self.write("lexicon/dictionary.tsv", "lemma\tpos\tgender\tgloss\tlevel\tregister\tnote\n"
                   "coger\tverb\t\tto take (Spain)\t\tvulgar\tVulgar in Mexico; say tomar or agarrar.\n"
                   "ordenador\tnoun\tm\tcomputer (Spain)\t\t\tIn Mexico: la computadora.\n")
        b, diag = self.compile()
        self.assertNotIn("peninsular", diag.codes(), [str(m) for m in diag.errors])
        self.assertIsNotNone(b.by_key["coger"].verb)
        # Without a note it is still refused, and sentences are still linted.
        self.replace("lexicon/dictionary.tsv", "\tIn Mexico: la computadora.\n", "\t\n")
        self.add_sentence("Ana tiene un [ordenador|ordenador].", "Ana has a computer.")
        _b, diag = self.compile()
        peninsular = [m for m in diag.errors if m.code == "peninsular"]
        self.assertEqual(len(peninsular), 2, [str(m) for m in peninsular])  # the bare row and the sentence

    def test_dictionary_review_pages(self):
        self.write("lexicon/dictionary-a.tsv", "lemma\tpos\tgender\tgloss\tlevel\tregister\tmx\tnote\n"
                   "abeja\tnoun\tf\tbee\t\t\t\t\n"
                   "chamaco\tnoun\tm\tkid\t\tinformal\tx\tAmong family and friends.\n")
        rev = os.path.join(self.tmp, "rev")
        result = self.packc("--check", "--review", "--review-dir", rev, "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(os.path.join(rev, "dictionary-a.html"), encoding="utf-8") as fh:
            page = fh.read()
        self.assertIn("Not required for release", page)
        self.assertLess(page.index("chamaco"), page.index("Other entries"))   # judgement calls first
        self.assertGreater(page.index("abeja"), page.index("Other entries"))
        with open(os.path.join(rev, "index.html"), encoding="utf-8") as fh:
            self.assertIn("how-to-review.html", fh.read())
        self.assertTrue(os.path.exists(os.path.join(rev, "how-to-review.html")))
        # Not gated for release, and signable with --mark-reviewed.
        b, _ = self.compile()
        status = {st.name: st for st in b.review_status}["dictionary-a"]
        self.assertFalse(status.gated or status.ok)
        result = self.packc("--mark-reviewed", "dictionary-a", "--by", "Rosa")
        self.assertEqual(result.returncode, 0, result.stderr)
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        self.assertTrue({st.name: st for st in b.review_status}["dictionary-a"].ok)
        self.replace("lexicon/dictionary-a.tsv", "kid", "kid; child")
        b, _ = self.compile()
        self.assertIn("changed since review", {st.name: st for st in b.review_status}["dictionary-a"].reason)

    def test_phrasebook_words(self):
        self.write("lexicon/phrasebook.tsv", "lemma\tpos\tgender\tgloss\tlevel\n"
                   "cuchara\tnoun\tf\tspoon\tA2\n")
        self.replace("phrasebook/basicos.yaml", "phrases:\n",
                     'phrases:\n  - es: "Una cuchara, por favor."\n    en: "A spoon, please."\n'
                     '    pron: "OO-nah koo-CHAH-rah pohr fah-BOHR"\n    note: "At the table."\n'
                     '    reviewer_notes: "Or una cucharita?"\n')
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])       # the phrase is cuchara's example
        entry = next(e for e in b.phrase_entries if b.sentences[e.sentence].es == "Una cuchara, por favor.")
        self.assertEqual(entry.pron, "OO-nah koo-CHAH-rah pohr fah-BOHR")
        item = next(i for i in b.items if i.kind == 5 and i.a == entry.sentence)
        self.assertEqual((item.lesson, bool(item.flags & 8)), (NONE, True))


NONE = 0xFFFF


class Linking(FixtureCase):
    def test_multiword_inside_a_clause_is_not_captured(self):
        self.append("lexicon/core.tsv", "nada\t\tpron\t\tnothing\tA1\t\t\tthings\t\t\t\n")
        self.add_sentence("Ana no come de nada.")
        b, diag = self.compile()
        self.assertIn("multiword", {m.code for m in diag.warnings})
        sent = next(s for s in b.sentences if s.es == "Ana no come de nada.")
        self.assertEqual([t.lemma.key for t in sent.tokens][-2:], ["de", "nada"])
        self.replace(LESSON, '"Ana no come de nada."', '"Ana no come [de nada|de nada]."')
        b, diag = self.compile()
        sent = next(s for s in b.sentences if s.es == "Ana no come de nada.")
        self.assertEqual(sent.tokens[-1].lemma.key, "de nada")
        self.assertNotIn("multiword", {m.code for m in diag.warnings})

    def test_shared_headword_warns_until_confirmed(self):
        self.add_sentence("El papa es bueno.")
        _b, diag = self.compile()
        self.assertIn("homograph", {m.code for m in diag.warnings})
        self.replace(LESSON, '"El papa es bueno."', '"El [papa|papa#pope] es bueno."')
        _b, diag = self.compile()
        self.assertNotIn("homograph", {m.code for m in diag.warnings})

    def test_headword_that_is_also_a_form_of_another_lemma_warns(self):
        self.append("lexicon/core.tsv", "trabajo\t\tnoun\tm\twork; job\tA1\t\t\twork\t\t\t\n")
        self.append("lexicon/core.tsv", "trabajar\t\tverb\t\tto work\tA1\t\t\twork\t\t\t\n")
        self.add_sentence("Trabajo en la casa.", "I work at home.")
        self.add_sentence("El trabajo es bueno.", "The job is good.")
        self.add_sentence("Ana trabaja en la casa.", "Ana works at home.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        warnings = [m.text for m in diag.warnings if m.code == "homograph"]
        self.assertEqual(len(warnings), 1, warnings)          # only the sentence-initial Trabajo
        self.assertIn("also a form of 'trabajar'", warnings[0])
        self.replace(LESSON, '"Trabajo en la casa."', '"[Trabajo|trabajar] en la casa."')
        _b, diag = self.compile()
        self.assertNotIn("homograph", {m.code for m in diag.warnings})

    def test_context_settles_inflected_forms(self):
        rows = ("enfermo\t\tadj\t\tsick\tA1\t\t\thealth\t\t\t\n"
                "enfermarse\t\tverb\t\tto get sick\tA2\t\t\thealth\t\t\t\n"
                "pasado\t\tadj\t\tlast; past\tA1\t\t\ttime\t\t\t\n"
                "pasar\t\tverb\t\tto pass; to happen\tA1\t\t\ttime\t\t\t\n"
                "sal\t\tnoun\tf\tsalt\tA1\t\t\tfood\tpl=sales\t\t\n"
                "salir\t\tverb\t\tto go out; to leave\tA1\t\t\tcity\t\t\t\n"
                "se\t\tpron\t\toneself; himself; herself\tA1\t\t\tgrammar\t\t\t\n"
                "las\t\tdet\tfpl\tthe (feminine plural)\tA1\t\t\tgrammar\t\t\t\n")
        self.append("lexicon/core.tsv", rows)
        self.append("verbs/irregular.yaml", "salir: {yo: salgo, fut: saldr, imp: {2s: sal}}\n")
        sentences = {
            "Ana está enferma.": ("enferma", "enfermo"),        # no se before it: the adjective
            "Ana se enferma.": ("enferma", "enfermarse"),       # its pronoun: the verb
            "La casa pasada es grande.": ("pasada", "pasado"),  # participle yields to adjective
            "Tú sales con Ana.": ("sales", "salir"),            # no determiner: the verb (warns)
            "Las sales son buenas.": ("sales", "sal"),          # after a determiner: the noun
            "Pasar es bueno.": ("Pasar", "pasar"),
        }
        for es in sentences:
            self.add_sentence(es)
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        links = {s.es: [(t.surface, t.lemma.key if t.lemma else None) for t in s.tokens] for s in b.sentences}
        for es, pair in sentences.items():
            self.assertIn(pair, links[es], es)
        warned = [m.text for m in diag.warnings if m.code == "homograph"]
        self.assertTrue(any("'sales' is linked to the verb 'salir'" in w for w in warned), warned)
        self.assertFalse(any("'Las'" in w or "'sales' is linked to 'sal'" in w for w in warned), warned)

    def test_verb_and_its_reflexive_twin(self):
        self.append("lexicon/core.tsv", "llamar\t\tverb\t\tto call\tA1\t\t\tpeople\t\t\t\n")
        self.add_sentence("Ana me llama.", "Ana calls me.")
        self.add_sentence("Tú te llamas Luis.", "Your name is Luis.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        links = {s.es: [(t.surface, t.lemma.key if t.lemma else None) for t in s.tokens] for s in b.sentences}
        self.assertIn(("llama", "llamar"), links["Ana me llama."])        # me is not the subject
        self.assertIn(("llamas", "llamarse"), links["Tú te llamas Luis."])
        self.assertIn(("llamo", "llamarse"), links["Hola, me llamo Ana."])  # fixture, still reflexive
        self.assertIn(("llamo", "llamarse"), links["¿Cómo te llamas? —Me llamo Luis."])

    def test_article_or_object_pronoun(self):
        self.append("lexicon/core.tsv",
                    "lo\t\tpron\t\tit; him (object)\tA2\t\t\tgrammar\tf=la; mpl=los; fpl=las\t\t\n")
        self.add_sentence("Ana lo come.", "Ana eats it.")
        self.add_sentence("Luis la come.", "Luis eats it.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        self.assertNotIn("homograph", {m.code for m in diag.warnings}, [str(m) for m in diag.warnings])
        links = {s.es: [(t.surface, t.lemma.key if t.lemma else None) for t in s.tokens] for s in b.sentences}
        self.assertIn(("la", "lo"), links["Luis la come."])                       # before a verb
        self.assertIn(("la", "la"), links["La computadora está en la casa."])     # before nouns

    def test_link_marked_rows(self):
        self.replace("lexicon/core.tsv", "\tnote\n", "\tnote\tlink\n")
        self.append("lexicon/core.tsv",
                    "salir\t\tverb\t\tto go out; to leave\tA1\t\t\tcity\t\t\t\t\n"
                    "sale\tsale#ok\tinterj\t\tOK!; deal!\tA1\tinformal\tx\tgreetings\t\t\t\tmarked\n")
        self.append("verbs/irregular.yaml", "salir: {yo: salgo, fut: saldr, imp: {2s: sal}}\n")
        self.replace(LESSON, "new: [casa,", "new: [sale#ok, casa,")
        self.add_sentence("Ana sale de la casa.", "Ana leaves the house.")
        self.add_sentence("¡{Sale|sale#ok}!", "OK!")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        self.assertNotIn("homograph", {m.code for m in diag.warnings})
        links = {s.es: [(t.surface, t.lemma.key) for t in s.tokens if t.lemma] for s in b.sentences}
        self.assertIn(("sale", "salir"), links["Ana sale de la casa."])   # unmarked: the verb, silently
        self.assertIn(("Sale", "sale#ok"), links["¡Sale!"])
        keys = {i.key for i in b.items}
        self.assertIn("vocab:sale#ok:recognise", keys)
        self.assertIn("cloze:sale:sale", keys)

    def test_punctuated_headword(self):
        self.append("lexicon/core.tsv",
                    "¿qué tal?\t\texpr\t\thow are you?; how's it going?\tA1\t\t\tgreetings\t\t\t\n")
        self.add_sentence("¿{Qué tal}, Luis?")
        self.add_sentence("¿{Mande}?")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        cloze = [i for i in b.items if i.kind == 2 and b.sentences[i.a].es in ("¿Qué tal, Luis?", "¿Mande?")]
        self.assertEqual(len(cloze), 2)
        for item in cloze:
            self.assertFalse(any(c[:1] in "¿¡" or c[-1:] in "?!" for c in item.candidates), item.candidates)

    def test_target_outside_lesson_sentences_warns(self):
        self.replace("stories/en-el-mercado.yaml", '"Luis vive en México."', '"Luis {vive} en México."')
        _b, diag = self.compile()
        self.assertIn("markup", {m.code for m in diag.warnings})

    def test_hay_links_to_haber(self):
        self.append("verbs/irregular.yaml", "haber:\n  extra: {hay: pres.3s}\n  forms: {pres.3s: ha}\n")
        self.append("lexicon/core.tsv",
                    "haber\t\tverb\t\tto have (auxiliary); there is, there are (hay)\tA1\t\t\tgrammar\t\t\t\n")
        self.add_sentence("Hay dos tacos.")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        sent = next(s for s in b.sentences if s.es == "Hay dos tacos.")
        self.assertEqual((sent.tokens[0].lemma.key, F.tag_name(sent.tokens[0].tag)), ("haber", "pres.3s"))
        self.assertEqual(b.by_key["haber"].form_for(F.verb_tag("pres", "3s")), "ha")


class Distractors(FixtureCase):
    def test_cloze_options_agree_with_the_sentence(self):
        b, _ = self.compile()
        by_key = {item.key: item for item in b.items}
        casa = by_key["cloze:ana vive en una casa grande:casa"]
        feminine = {lem.es for lem in b.lemmas if lem.pos == 1 and lem.gender == 2} | {
            lem.feminine for lem in b.lemmas if lem.feminine}
        self.assertTrue(set(casa.candidates) <= feminine, casa.candidates)
        tacos = by_key["cloze:luis come dos tacos:tacos"]
        masculine_plurals = {lem.plural for lem in b.lemmas if lem.pos == 1 and lem.gender in (1, 3)} | {
            lem.es for lem in b.lemmas if lem.plural_only and lem.gender == 1}
        self.assertTrue(set(tacos.candidates) <= masculine_plurals, tacos.candidates)
        self.assertGreaterEqual(len(tacos.candidates), 3)

    def test_options_fit_the_article(self):
        b, _ = self.compile()
        by_key = {item.key: item for item in b.items}
        takes_el = {lem.es for lem in b.lemmas if lem.pos == 1 and not lem.plural_only
                    and (lem.gender in (1, 3) or lem.flags & 4)}
        # El [agua]: a feminine noun taking el, so masculine nouns or others like it.
        agua = by_key["cloze:el agua esta muy fria:agua"].candidates
        self.assertTrue(set(agua) <= takes_el, agua)
        self.assertNotIn("casa", agua)
        # El [camión] may be offered agua; una [casa] never is (una agua).
        self.assertIn("agua", by_key["cloze:el camion esta aqui:camion"].candidates)
        casa = by_key["cloze:ana vive en una casa grande:casa"].candidates
        self.assertNotIn("agua", casa)
        self.assertGreaterEqual(len(casa), 3)

    def test_synonyms_are_never_distractors(self):
        b, _ = self.compile()
        hola = next(i for i in b.items if i.key == "vocab:hola:recognise")
        self.assertIn(b.by_key["adiós"].id, hola.candidates)
        self.replace("lexicon/core.tsv", "\tnote\n", "\tnote\tsyn\n")
        self.replace("lexicon/core.tsv", "hi; hello\tA1\t\t\tgreetings\t\t\t\n",
                     "hi; hello\tA1\t\t\tgreetings\t\t\t\tadiós\n")
        b, diag = self.compile()
        self.assertEqual([str(m) for m in diag.errors], [])
        for key, other in (("vocab:hola:recognise", "adiós"), ("vocab:adiós:produce", "hola")):
            item = next(i for i in b.items if i.key == key)
            self.assertNotIn(b.by_key[other].id, item.candidates, key)


class RoundTrip(FixtureCase):
    def test_pack_reads_back(self):
        b, diag = self.compile()
        data, _report = emit(b)
        pack = Pack(data)
        self.assertTrue(pack.crc_ok())
        self.assertEqual(len(data) % 4, 0)
        self.assertEqual(pack.count("LEMM"), len(b.lemmas))
        self.assertEqual(pack.count("ITEM"), len(b.items))
        for lid, lemma in enumerate(pack.records("LEMM")):
            self.assertEqual(pack.str(lemma["es"]), b.lemmas[lid].es)
        keys = [pack.str(k["key"]) for k in pack.records("LKEY")]
        self.assertEqual(keys, sorted(keys))
        toks = pack.records("TOKS")
        for sent, src in zip(pack.records("SENT"), b.sentences):
            raw = pack.str(sent["es"]).encode("utf-8")
            for t, tok in zip(toks[sent["firstToken"]:sent["firstToken"] + sent["tokenCount"]], src.tokens):
                surface = raw[t["start"]:t["start"] + t["length"]].decode("utf-8")
                self.assertEqual(surface, tok.surface)
        uids = [r["uid"] for r in pack.records("IUID")]
        self.assertEqual(uids, sorted(uids))

    def test_em_dash_uses_charset_slot(self):
        b, _diag = self.compile()
        data, _ = emit(b)
        pack = Pack(data)
        texts = [pack.str(s["es"]) for s in pack.records("SENT")]
        self.assertTrue(any("\x97" in t for t in texts))
        self.assertFalse(any("—" in t for t in texts))

    def test_linking(self):
        b, _diag = self.compile()
        by_text = {s.es: s for s in b.sentences}
        sent = by_text["Ayúdeme, por favor."]
        self.assertEqual([t.surface for t in sent.tokens], ["Ayúdeme", "por favor"])
        self.assertEqual(sent.tokens[0].lemma.key, "ayudar")
        self.assertEqual(sent.tokens[0].tag, F.verb_tag("imp", "3s") | F.ENCLITIC)
        sent = by_text["El papa vive en Roma."]
        self.assertEqual(sent.tokens[1].lemma.key, "papa#pope")
        sent = by_text["Son 20 pesos."]
        self.assertIsNone(sent.tokens[1].lemma)
        self.assertEqual(sent.tokens[2].tag, F.NOMINAL["pl"])
        agua = b.by_key["agua"]
        self.assertTrue(agua.flags & 4)  # el agua
        self.assertEqual(b.by_key["bueno"].form_for(F.APOCOPE), "buen")

    def test_items(self):
        b, _diag = self.compile()
        kinds = {item.kind for item in b.items}
        self.assertEqual(kinds, set(range(7)))
        by_key = {item.key: item for item in b.items}
        rec = by_key["vocab:casa:recognise"]
        self.assertEqual(by_key["vocab:casa:produce"].prereq, rec.index)
        self.assertEqual(by_key["gender:casa"].prereq, rec.index)
        self.assertNotIn("gender:amigo", by_key)  # m/f nouns have no gender drill
        cloze = by_key["cloze:usted es de aqui:es"]
        self.assertEqual(cloze.must_show, 1)  # estar's form leads, from the confusable set
        self.assertEqual(cloze.candidates[0], "está")
        for lesson in b.lessons:
            new_items = b.items[lesson.first_item:lesson.first_item + len(lesson.new)]
            self.assertTrue(all(i.kind == 0 for i in new_items))


def meaning(b, item) -> tuple:
    """What an item asks, independent of ids and positions."""
    if item.kind in (2, 5, 6):
        sent = b.sentences[item.a]
        target = sent.tokens[item.b].surface if item.kind == 2 else ""
        return item.kind, sent.es, target
    return item.kind, b.lemmas[item.a].key, item.b


VIVE = '  - es: "Ana {vive|vivir:pres.3s} en una {casa} grande."\n    en: "Ana lives in a big house."\n'
COME = '  - es: "Luis {come|comer:pres.3s} dos {tacos}."\n'


class IdLock(FixtureCase):
    def build(self):
        result = self.packc("--out", os.path.join(self.tmp, "x.pack"), "--quiet")
        self.assertEqual(result.returncode, 0, result.stderr)
        b, _ = self.compile()
        return {item.uid: meaning(b, item) for item in b.items}

    def test_inserting_and_reordering_keeps_every_uid(self):
        first = self.build()
        # A sentence with the same target lemma and form as an existing one, inserted
        # before it; then the lesson's first two sentences swapped.
        self.add_sentence("Luis {come|comer:pres.3s} en la casa.", "Luis eats in the house.")
        self.replace(LESSON, VIVE, "")
        self.replace(LESSON, COME, VIVE + COME)
        second = self.build()
        for uid, what in first.items():
            self.assertEqual(second.get(uid), what, uid)
        self.assertEqual(len(second), len(first) + 1)  # the new sentence's cloze

    def test_reserved_uid_is_never_assigned(self):
        from packc.items import RESERVED_UID, IdLock as Lock
        lock = Lock(os.path.join(self.tmp, "none.lock"), Diagnostics())
        lock.next_uid = RESERVED_UID - 1
        self.assertEqual(lock.uid_for("a"), RESERVED_UID - 1)
        with self.assertRaises(ValueError):
            lock.uid_for("b")

    def test_edited_or_deleted_line_is_an_error(self):
        self.build()
        lock = self.path("ids.lock")
        with open(lock, encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        edited = [line.replace("vocab:casa:", "vocab:mesa:") for line in lines]
        with open(lock, "w", encoding="utf-8") as fh:
            fh.write("\n".join(edited))
        _b, diag = self.compile()
        self.assertTrue(any("checksum" in m.text for m in diag.errors))
        data_lines = [i for i, line in enumerate(lines) if line and not line.startswith("#")]
        del lines[data_lines[3]]
        with open(lock, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        _b, diag = self.compile()
        self.assertTrue(any("out of sequence" in m.text for m in diag.errors))


class Review(FixtureCase):
    def test_mark_reviewed(self):
        result = self.packc("--mark-reviewed", "u01.l01", "--by", "Rosa Pérez")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("u01.l01", result.stdout)
        b, _ = self.compile()
        status = {s.name: s.ok for s in b.review_status}
        self.assertTrue(status["u01.l01"])
        self.assertFalse(status["u01.l02"])
        with open(self.path("units/01-saludos/lesson-1.yaml"), encoding="utf-8") as fh:
            self.assertIn('reviewed: {by: "Rosa Pérez"', fh.read())
        # Marking again replaces the entry instead of adding a second one.
        result = self.packc("--mark-reviewed", "u01.l01", "--by", "Rosa")
        self.assertEqual(result.returncode, 0, result.stderr)
        _b, diag = self.compile()
        self.assertNotIn("yaml", diag.codes())
        self.assertEqual(self.packc("--mark-reviewed", "u09.l09", "--by", "X").returncode, 2)
        self.assertEqual(self.packc("--mark-reviewed", "all").returncode, 2)  # no --by

    def test_hash_and_release(self):
        result = self.packc("--mark-reviewed", "all", "--by", "Tester")
        self.assertEqual(result.returncode, 0, result.stderr)
        b, diag = self.compile()
        self.assertTrue(all(s.ok for s in b.review_status), [s.reason for s in b.review_status])
        self.assertTrue(all(lesson.reviewed for lesson in b.lessons))
        result = self.packc("--out", os.path.join(self.tmp, "seed.pack"), "--quiet")  # assigns ids
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.packc("--release", "--out", os.path.join(self.tmp, "r.pack"))
        self.assertEqual(result.returncode, 0, result.stderr)
        with open(os.path.join(self.tmp, "r.pack"), "rb") as fh:
            self.assertTrue(Pack(fh.read()).flags & 1)
        self.replace(LESSON, "Ana lives in a big house.", "Ana lives in a large house.")
        result = self.packc("--release", "--out", os.path.join(self.tmp, "r2.pack"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("changed since review", result.stderr)
        result = self.packc("--check")
        self.assertEqual(result.returncode, 0)
        self.assertIn("--release would fail", result.stderr)


class Units(unittest.TestCase):
    def test_fold(self):
        self.assertEqual(fold("¿Qué onda?"), "que onda")
        self.assertEqual(fold("  Niño\tAÑO "), "nino ano")
        self.assertEqual(fold("¡Órale!"), "orale")
        self.assertEqual(fold("Straße"), "strasse")

    def test_sentence_markup(self):
        text, marks = markup.parse_sentence("¿Me {trae|traer:pres.3s} dos [tacos|taco]?")
        self.assertEqual(text, "¿Me trae dos tacos?")
        self.assertEqual((marks[0].lemma, marks[0].form, marks[0].target), ("traer", "pres.3s", True))
        self.assertEqual((marks[1].lemma, marks[1].form, marks[1].target), ("taco", None, False))
        for bad in ("{abc", "a}b", "{a{b}}", "{|x}", "{ a}"):
            with self.assertRaises(markup.MarkupError, msg=bad):
                markup.parse_sentence(bad)

    def test_note_markup(self):
        spans = markup.parse_note("Use _tú_ with *friends*: `TOO`.\n\n- ~vosotros~ is _*not*_ used")
        styles = [s for s, _t in spans]
        self.assertEqual(styles, [markup.TEXT, markup.SPANISH, markup.TEXT, markup.STRONG, markup.TEXT,
                                  markup.RESPELLING, markup.TEXT, markup.BULLET, markup.NOT_MEXICAN,
                                  markup.TEXT, markup.SPANISH_STRONG, markup.TEXT])

    def test_tags(self):
        self.assertEqual(F.parse_tag("pres.3s"), 0x1002)
        self.assertEqual(F.parse_tag("impneg.3p"), 0x1074)
        self.assertEqual(F.tag_name(0x1162), "imp.3s+clitic")
        with self.assertRaises(F.TagError):
            F.parse_tag("pres.2p")
        with self.assertRaises(F.TagError):
            F.parse_tag("imp.1s")

    def test_inflection(self):
        cases = {"joven": "jóvenes", "camión": "camiones", "lápiz": "lápices", "inglés": "ingleses",
                 "árbol": "árboles", "lunes": "lunes", "café": "cafés", "autobús": "autobuses",
                 "examen": "exámenes", "mes": "meses", "rey": "reyes", "país": "países",
                 "maíz": "maíces", "baúl": "baúles", "régimen": "regímenes", "carácter": "caracteres"}
        for word, plural in cases.items():
            self.assertEqual(F.pluralize(word), plural, word)
        self.assertEqual(F.feminine("chido"), "chida")
        self.assertEqual(F.feminine("trabajador"), "trabajadora")
        self.assertEqual(F.feminine("inglés"), "inglesa")
        self.assertIsNone(F.feminine("grande"))
        self.assertIsNone(F.feminine("mejor"))


if __name__ == "__main__":
    unittest.main()
