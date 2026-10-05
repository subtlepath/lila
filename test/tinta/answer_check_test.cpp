// modules: lang
//
// Typed-answer checking (PLAN.md 8.4): normalisation, accents, one-letter
// typos on long words, accepted non-Mexican synonyms, several right answers.

#include <cstring>
#include <string>

#include "check.h"
#include "core/lang/AnswerCheck.h"

using namespace tinta::core::lang;

namespace {

std::string normalized(const char* s) {
  char out[kMaxAnswerBytes + 1];
  normalizeAnswer(s, std::strlen(s), out, sizeof out);
  return out;
}

std::string folded(const char* s) {
  char out[kMaxAnswerBytes + 1];
  foldAnswer(s, std::strlen(s), out, sizeof out);
  return out;
}

void testNormalize() {
  CHECK(normalized("  ¿Dónde   ESTÁ?  ") == "dónde está");
  CHECK(normalized("¡Órale!") == "órale");
  CHECK(normalized("Ñandú.") == "ñandú");
  CHECK(normalized("e-mail, por favor") == "email por favor");
  CHECK(normalized("") == "");
  CHECK(normalized("¿?") == "");
  CHECK(folded("¿Dónde está?") == "donde esta");
  CHECK(folded("Año Ñu") == "ano nu");
  CHECK(folded("pingüino") == "pinguino");
  CHECK(folded("Straße") == "strasse");
}

void testEditDistance() {
  CHECK_EQ(editDistance("casa", 4, "casa", 4, 3), 0);
  CHECK_EQ(editDistance("casa", 4, "cosa", 4, 3), 1);
  CHECK_EQ(editDistance("casa", 4, "casas", 5, 3), 1);
  CHECK_EQ(editDistance("casa", 4, "asa", 3, 3), 1);
  CHECK_EQ(editDistance("kitten", 6, "sitting", 7, 5), 3);
  CHECK_EQ(editDistance("kitten", 6, "sitting", 7, 1), 2);  // past the limit
  CHECK_EQ(editDistance("", 0, "abc", 3, 5), 3);
}

void testVerdicts() {
  CHECK(checkAnswer("está", "está") == Verdict::Exact);
  CHECK(checkAnswer("  Está. ", "está") == Verdict::Exact);
  CHECK(checkAnswer("¿Dónde está?", "dónde está") == Verdict::Exact);
  CHECK(checkAnswer("esta", "está") == Verdict::Accents);
  CHECK(checkAnswer("ESTA", "está") == Verdict::Accents);
  CHECK(checkAnswer("ano", "año") == Verdict::Accents);
  CHECK(checkAnswer("no", "año") == Verdict::Wrong);
  // One edit on six letters or more is a typo; on a short word it is wrong.
  CHECK(checkAnswer("computadra", "computadora") == Verdict::Typo);
  CHECK(checkAnswer("chamarar", "chamarra") == Verdict::Wrong);  // a transposition is two edits
  CHECK(checkAnswer("chamara", "chamarra") == Verdict::Typo);
  CHECK(checkAnswer("caso", "casa") == Verdict::Wrong);
  CHECK(checkAnswer("compudora", "computadora") == Verdict::Wrong);
  // Non-Mexican synonyms are accepted, accents or not.
  CHECK(checkAnswer("ordenador", "computadora", "ordenador; computador") == Verdict::Alternative);
  CHECK(checkAnswer("Móvil", "celular", "móvil") == Verdict::Alternative);
  CHECK(checkAnswer("movil", "celular", "móvil") == Verdict::Alternative);
  CHECK(checkAnswer("coche", "carro", nullptr) == Verdict::Wrong);
  // Several right answers.
  CHECK(checkAnswer("lentes", "lentes; anteojos") == Verdict::Exact);
  CHECK(checkAnswer("anteojos", "lentes; anteojos") == Verdict::Exact);
  // Nothing typed, or only punctuation, is wrong, never exact.
  CHECK(checkAnswer("", "sí") == Verdict::Wrong);
  CHECK(checkAnswer("¡!", "sí") == Verdict::Wrong);
  CHECK(checkAnswer(nullptr, "sí") == Verdict::Wrong);
  CHECK(checkAnswer("si", nullptr) == Verdict::Wrong);
  // Multi-word answers keep their spaces.
  CHECK(checkAnswer("por favor", "por favor") == Verdict::Exact);
  CHECK(checkAnswer("porfavor", "por favor") == Verdict::Typo);
  CHECK(checkAnswer("me llamo", "me llamo") == Verdict::Exact);

  CHECK(accepted(Verdict::Typo) && shaky(Verdict::Typo));
  CHECK(accepted(Verdict::Alternative) && !shaky(Verdict::Alternative));
  CHECK(!accepted(Verdict::Wrong));
}

void testLongInput() {
  // Longer than the buffers: cut, never overrun.
  std::string long1(400, 'a');
  std::string long2(400, 'a');
  CHECK(checkAnswer(long1.c_str(), long2.c_str()) == Verdict::Exact);
  std::string accents;
  for (int i = 0; i < 100; ++i) accents += "á";
  CHECK(normalized(accents.c_str()).size() <= kMaxAnswerBytes);
}

}  // namespace

int main() {
  testNormalize();
  testEditDistance();
  testVerdicts();
  testLongInput();
  return tinta_test::result();
}
