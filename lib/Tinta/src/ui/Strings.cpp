#include "ui/Strings.h"

#include <stdio.h>

#include "core/Date.h"
#include "core/lang/Charset.h"

namespace tinta::ui {
namespace {

struct Row {
  const char* en;
  const char* es;
};

// In Str order.
const Row kRows[] = {
    {"Tinta", "Tinta"},
    {"Home", "Inicio"},
    {"Settings", "Ajustes"},
    {"Back", "Atrás"},
    {"Select", "Elegir"},
    {"Open", "Abrir"},
    {"Change", "Cambiar"},
    {"Resume", "Continuar"},
    {"Done", "Listo"},
    {"Save", "Guardar"},
    {"Cancel", "Cancelar"},
    {"On", "Sí"},
    {"Off", "No"},
    {"Paused", "En pausa"},
    {"Light", "Luz"},
    // Storage
    {"No SD card " TINTA_EM_DASH " progress will not be saved",
     "Sin tarjeta SD " TINTA_EM_DASH " el progreso no se guardará"},
    {"The SD card stopped responding " TINTA_EM_DASH " progress is not being saved",
     "La tarjeta SD dejó de responder " TINTA_EM_DASH " no se guarda el progreso"},
    // Settings groups
    {"Study", "Estudio"},
    {"Display", "Pantalla"},
    {"Date and time", "Fecha y hora"},
    {"About", "Acerca de Tinta"},
    {"Diagnostics", "Diagnóstico"},
    // Study
    {"New words per day", "Palabras nuevas al día"},
    {"Daily review limit", "Límite de repasos al día"},
    {"Target retention", "Retención deseada"},
    {"Longest interval", "Intervalo máximo"},
    {"Session length", "Duración de la sesión"},
    {"Show vulgar words", "Mostrar palabras vulgares"},
    {"%u items", "%u tarjetas"},
    {"%u days", "%u días"},
    {"%u%%", "%u %%"},
    // Display
    {"Text size", "Tamaño del texto"},
    {"Small", "Pequeño"},
    {"Medium", "Mediano"},
    {"Large", "Grande"},
    {"Interface language", "Idioma de la interfaz"},
    {"English", "English"},
    {"Español", "Español"},
    {"Full refresh every", "Refresco completo cada"},
    {"%u screens", "%u pantallas"},
    // Time
    {"Today" TINTA_RSQUO "s date", "Fecha de hoy"},
    {"New day starts at", "El día empieza a las"},
    {"Year", "Año"},
    {"Month", "Mes"},
    {"Day", "Día"},
    {"Set today" TINTA_RSQUO "s date", "Fijar la fecha de hoy"},
    // Date prompt
    {"What day is it?", "¿Qué día es hoy?"},
    {"Same day", "Mismo día"},
    {"Next day", "Siguiente"},
    {"Other date", "Otra fecha"},
    {"Last time: %s", "La última vez: %s"},
    // First run
    {"Welcome to Tinta", "Te damos la bienvenida a Tinta"},
    {"Tell Tinta today" TINTA_RSQUO "s date to begin. It will ask each time you turn it on.",
     "Indica la fecha de hoy para empezar. Tinta la preguntará cada vez que la enciendas."},
    // About and diagnostics
    {"Version", "Versión"},
    {"Device", "Dispositivo"},
    {"Panel", "Pantalla"},
    {"SD card", "Tarjeta SD"},
    {"Ready", "Lista"},
    {"Absent (guest mode)", "Ausente (modo invitado)"},
    {"Stopped responding", "Dejó de responder"},
    {"Clock", "Reloj"},
    {"Real-time clock", "Reloj de tiempo real"},
    {"Asked at power-on", "Se pregunta al encender"},
    {"Current lesson", "Lección actual"},
    {"Unlocked through", "Desbloqueado hasta"},
    {"Board profile", "Perfil de placa"},
    // Course pack
    {"Course", "Curso"},
    {"Edition %lu", "Edición %lu"},
    {"Edition %lu, %lu items", "Edición %lu, %lu tarjetas"},
    {"Check course data", "Revisar datos del curso"},
    {"Intact", "Correctos"},
    {"Damaged", "Dañados"},
    {"Course data missing", "Faltan los datos del curso"},
    {"The course could not be read (%s). Copy course.pack into the tinta folder of the SD card and open Tinta again.",
     "No se pudo leer el curso (%s). Copia course.pack en la carpeta tinta de la tarjeta SD y abre Tinta de nuevo."},
    // Home: Today
    {"Today", "Hoy"},
    {"%u reviews · %u new", "%u repasos · %u nuevas"},
    {"%u left in this session", "Quedan %u en esta sesión"},
    {"All done for today", "Todo listo por hoy"},
    {"Streak: %u days", "Racha: %u días"},
    {"Streak: 1 day", "Racha: 1 día"},
    {"1 day", "1 día"},
    {"Start", "Empezar"},
    {"Continue", "Continuar"},
    {"Dictionary", "Diccionario"},
    {"Progress", "Progreso"},
    // Session
    {"Review %u/%u", "Repaso %u/%u"},
    {"Again", "Otra vez"},
    {"Hard", "Difícil"},
    {"Good", "Bien"},
    {"Easy", "Fácil"},
    {"this session", "esta sesión"},
    {"%u d", "%u d"},
    {"%u mo", "%u m"},
    {"%u.%u y", "%u,%u a"},
    {"End session", "Terminar sesión"},
    {"Session complete", "Sesión terminada"},
    {"Reviewed", "Repasadas"},
    {"Correct", "Correctas"},
    {"New", "Nuevas"},
    {"Time", "Tiempo"},
    {"Streak", "Racha"},
    {"A tricky one", "Una difícil"},
    {"This one has slipped away %u times. Suspend it for now, or keep it in your reviews?",
     "Se te ha olvidado %u veces. ¿La suspendes por ahora o la sigues repasando?"},
    {"Suspend", "Suspender"},
    {"Keep", "Seguir"},
    {"In Spanish?", "¿En español?"},
    {"el or la?", "¿el o la?"},
    {"Fill the gap", "Completa la frase"},
    {"Put the words in order", "Ordena las palabras"},
    {"How do you say it?", "¿Cómo se dice?"},
    {"Conjugate", "Conjuga"},
    {"What does it mean?", "¿Qué significa?"},
    {"Right", "Correcto"},
    {"The answer: %s", "La respuesta: %s"},
    {"Right " TINTA_EM_DASH " mind the accents", "Correcto " TINTA_EM_DASH " ojo con los acentos"},
    {"Right " TINTA_EM_DASH " check the spelling", "Correcto " TINTA_EM_DASH " revisa la ortografía"},
    {"Right " TINTA_EM_DASH " in Mexico: %s", "Correcto " TINTA_EM_DASH " en México: %s"},
    {"You typed: %s", "Escribiste: %s"},
    {"Press any key to go on", "Presiona cualquier tecla para seguir"},
    {"Tap to go on", "Toca para seguir"},
    {"1 mistake", "1 error"},
    {"%u mistakes", "%u errores"},
    {"Place", "Poner"},
    {"Type the answer", "Escribe la respuesta"},
    {"Check", "Revisar"},
    {"Type answers", "Escribir las respuestas"},
    // Parts of speech
    {"NOUN", "SUSTANTIVO"},
    {"VERB", "VERBO"},
    {"ADJECTIVE", "ADJETIVO"},
    {"ADVERB", "ADVERBIO"},
    {"PRONOUN", "PRONOMBRE"},
    {"DETERMINER", "DETERMINANTE"},
    {"PREPOSITION", "PREPOSICIÓN"},
    {"CONJUNCTION", "CONJUNCIÓN"},
    {"INTERJECTION", "INTERJECCIÓN"},
    {"NUMBER", "NUMERAL"},
    {"EXPRESSION", "EXPRESIÓN"},
    {"NAME", "NOMBRE PROPIO"},
    // Dictionary
    {"A" TINTA_EN_DASH "Z", "A" TINTA_EN_DASH "Z"},
    {"Jump to a letter", "Ir a una letra"},
    {"Conjugation", "Conjugación"},
    {"Gerund", "Gerundio"},
    {"Participle", "Participio"},
    {"%u/%u", "%u/%u"},
    // Progress
    {"Not started", "Sin empezar"},
    {"Learning", "Aprendiendo"},
    {"Mature", "Maduras"},
    {"Suspended", "Suspendidas"},
    {"Reviews, last 14 days", "Repasos, últimos 14 días"},
    {"Due, next 14 days", "Pendientes, próximos 14 días"},
    {"Time studied: %u h %02u min", "Tiempo de estudio: %u h %02u min"},
    {"%lu days studied", "%lu días de estudio"},
    // Lessons
    {"Lesson %s", "Lección %s"},
    {"Lesson %s: %s", "Lección %s: %s"},
    {"Lesson %s · %u/%u", "Lección %s · %u/%u"},
    {"Course map", "Mapa del curso"},
    {"New word %u of %u", "Palabra nueva %u de %u"},
    {"English", "Inglés"},
    {"Narrator", "Narrador"},
    {"Practice", "Práctica"},
    {"%u items to practise", "%u tarjetas para practicar"},
    {"Answer each one; the words come back until you know them. Finishing the practice opens the next lesson.",
     "Contesta cada una; las palabras vuelven hasta que te las sepas. Al terminar la práctica se abre la "
     "siguiente lección."},
    {"Lesson %s complete", "Lección %s terminada"},
    {"Next: Lesson %s, %s", "Siguiente: Lección %s, %s"},
    {"You have finished every lesson.", "Terminaste todas las lecciones."},
    {"locked", "bloqueada"},
    {"Unlock all lessons", "Desbloquear todas las lecciones"},
    {"Unit %u · %s", "Unidad %u · %s"},
    {"In this lesson", "En esta lección"},
    {"%u new words", "%u palabras nuevas"},
    {"Dialogue", "Diálogo"},
    {"Grammar", "Gramática"},
    {"Culture", "Cultura"},
    {"Pronunciation", "Pronunciación"},
    {"Usage", "Uso"},
    {"Start practice", "Empezar la práctica"},
    {"now", "ahora"},
    {"Next", "Siguiente"},
    {"Next lesson", "Siguiente lección"},
    // Key names
    {"Back", "Atrás"},
    // M6
    {"Remember this one?", "¿Te acuerdas?"},
    {"A word from your lesson", "Una palabra de tu lección"},
    {"Tomorrow: %lu to review", "Mañana: %lu por repasar"},
    {"Read", "Leer"},
    {"Phrases", "Frases"},
    {"Readings", "Lecturas"},
    {"Phrases · %u/%u", "Frases · %u/%u"},
    {"%u phrases", "%u frases"},
    {"Practise", "Practicar"},
    {"Questions", "Preguntas"},
    {"Then %u questions", "Luego %u preguntas"},
    {"The end", "Fin"},
    {"Not quite", "No exactamente"},
    {"Question %u of %u", "Pregunta %u de %u"},
    {"%u of %u right", "%u de %u correctas"},
    {"Gloss", "Ver"},
    {"Close", "Cerrar"},
    {"Add to my deck", "Agregar a mi mazo"},
    {"Take out of my deck", "Quitar de mi mazo"},
    {"Add", "Agregar"},
    {"Remove", "Quitar"},
    {"In deck", "En mazo"},
    {"In your deck: it comes up in the next session.", "En tu mazo: sale en la próxima sesión."},
    {"Learnt", "Aprendida"},
    {"Options", "Opciones"},
    {"Search", "Buscar"},
    {"A word in Spanish or English", "Una palabra en español o en inglés"},
    {"Nothing found", "No se encontró nada"},
    {"More: keep typing", "Hay más: sigue escribiendo"},
    {"Dictionary · English", "Diccionario · inglés"},
    {"English words from:", "Palabras en inglés desde:"},
    {"Del", "Borrar"},
    // M7 interface
    {"Press any key to show the answer", "Presiona cualquier tecla para ver la respuesta"},
    {"Tap to show the answer", "Toca para ver la respuesta"},
    {"Auto (English)", "Automático (inglés)"},
    {"Auto (Spanish)", "Automático (español)"},
    {"Choose the language of the menus. Auto starts in English and switches to Spanish once you finish Unit 4.",
     "Elige el idioma de los menús. Automático empieza en inglés y cambia a español cuando terminas la "
     "unidad 4."},
    {"How the keys work", "Cómo funcionan las teclas"},
    {"The four keys under the screen do what the labels above them say. In an exercise, the key under an answer "
     "chooses it.",
     "Las cuatro teclas bajo la pantalla hacen lo que dicen las etiquetas de arriba. En un ejercicio, la tecla "
     "bajo una respuesta la elige."},
    {"The side keys move through lists and turn pages. In a review, the up key takes back your last answer.",
     "Las teclas laterales recorren las listas y pasan las páginas. En un repaso, la tecla de subir deshace tu "
     "última respuesta."},
    {"Hold %s for the menu: Home, Settings and Sleep.", "Mantén presionada %s para el menú: Inicio, Ajustes y Dormir."},
    {"Power puts Tinta to sleep. Press it again to carry on where you left off.",
     "El botón de encendido pone a dormir a Tinta. Presiónalo de nuevo para seguir donde te quedaste."},
    {"How to use the screen", "Cómo usar la pantalla"},
    {"Tap what you want. The arrow at the top left goes back.",
     "Toca lo que quieras. La flecha de arriba a la izquierda regresa."},
    {"In an exercise, tap an answer. Tap the card to show the answer or to go on; swipe right to take back your "
     "last answer.",
     "En un ejercicio, toca una respuesta. Toca la tarjeta para ver la respuesta o para seguir; desliza a la "
     "derecha para deshacer tu última respuesta."},
    {"Swipe down from the top edge for the light.", "Desliza hacia abajo desde el borde de arriba para la luz."},
    {"Tap the Home pad under the screen for the menu: Home, Settings and Sleep. Hold it to go straight Home.",
     "Toca el botón de inicio bajo la pantalla para el menú: Inicio, Ajustes y Dormir. Mantenlo presionado para "
     "ir directo a Inicio."},
    {"Vulgar words", "Palabras vulgares"},
    {"Mexican Spanish is full of slang, and some of it is rude. Tinta leaves vulgar words out of reviews and "
     "practice, and hides their meaning in the dictionary, unless you turn them on. You can change this later in "
     "Settings > Study.",
     "El español de México tiene mucha jerga, y parte de ella es grosera. Tinta deja las palabras vulgares fuera "
     "de los repasos y la práctica, y oculta su significado en el diccionario, a menos que las actives. Puedes "
     "cambiarlo después en Ajustes > Estudio."},
    {"Licences", "Licencias"},
    {"Times and Helvetica", "Times y Helvetica"},
    {"Larger type", "Tipos más grandes"},
    {"Software", "Software"},
    {"MIT License", "Licencia MIT"},
    {"The lessons, dialogues, readings, phrasebook, glosses and notes are written for Tinta. The order of the "
     "frequency deck is derived from word counts in the OpenSubtitles 2018 Spanish list of the FrequencyWords "
     "project by Hermit Dave (github.com/hermitdave/FrequencyWords), whose list is licensed CC BY-SA 4.0. Only a "
     "ranking was derived; no text of the list is reproduced.",
     "Las lecciones, los diálogos, las lecturas, las frases, las glosas y las notas están escritos para Tinta. El "
     "orden del mazo de frecuencia se deriva del recuento de palabras de la lista en español de OpenSubtitles "
     "2018 del proyecto FrequencyWords de Hermit Dave (github.com/hermitdave/FrequencyWords), cuya lista tiene "
     "licencia CC BY-SA 4.0. Solo se derivó una clasificación; no se reproduce ningún texto de la lista."},
    {"Leave", "Salir"},
};
static_assert(sizeof kRows / sizeof kRows[0] == static_cast<size_t>(Str::Count), "one row per Str");

const char* const kMonthShort[2][12] = {
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
    {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"},
};
const char* const kMonthName[2][12] = {
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
     "December"},
    {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre",
     "diciembre"},
};
const char* const kWeekday[2][7] = {
    {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"},
    {"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"},
};

const char* const kWeekdayShort[2][7] = {
    {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"},
    {"lun", "mar", "mié", "jue", "vie", "sáb", "dom"},
};

core::UiLanguage gLanguage = core::UiLanguage::English;

int column() { return gLanguage == core::UiLanguage::Spanish ? 1 : 0; }

}  // namespace

void setLanguage(const core::UiLanguage language) {
  gLanguage = language == core::UiLanguage::Spanish ? language : core::UiLanguage::English;
}

core::UiLanguage language() { return gLanguage; }

const char* tr(const Str id) { return trIn(id, gLanguage); }

const char* trIn(const Str id, const core::UiLanguage language) {
  const auto i = static_cast<size_t>(id);
  if (i >= static_cast<size_t>(Str::Count)) return "";
  return language == core::UiLanguage::Spanish ? kRows[i].es : kRows[i].en;
}

const char* monthShort(const uint8_t month) {
  return month >= 1 && month <= 12 ? kMonthShort[column()][month - 1] : "";
}

const char* monthName(const uint8_t month) { return month >= 1 && month <= 12 ? kMonthName[column()][month - 1] : ""; }

const char* weekdayName(const uint8_t weekday) { return weekday < 7 ? kWeekday[column()][weekday] : ""; }

void formatDate(const core::DayNumber day, const DateStyle style, char* out, const size_t cap) {
  const core::date::Civil c = core::date::civil(day);
  const uint8_t wd = core::date::weekday(day);
  const int col = column();
  if (style == DateStyle::Short) {
    snprintf(out, cap, "%s %u %s", kWeekdayShort[col][wd], c.day, kMonthShort[col][c.month - 1]);
  } else if (col == 1) {
    snprintf(out, cap, "%s %u de %s de %u", kWeekday[col][wd], c.day, kMonthName[col][c.month - 1], c.year);
  } else {
    snprintf(out, cap, "%s %u %s %u", kWeekday[col][wd], c.day, kMonthName[col][c.month - 1], c.year);
  }
}

}  // namespace tinta::ui
