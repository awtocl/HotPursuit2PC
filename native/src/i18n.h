// Languages of the port's own text (ADR 0017): the launcher and the in-game settings menu. The game's own text is
// the game's (this North American disc shows it in English). English is the source; every other language comes from
// the table in i18n.cpp, and a string missing there stays in English.
#pragma once
#include <initializer_list>
#include <string>

namespace i18n {

enum Lang : int { kEnglish, kSpanish, kPortuguese, kFrench, kGerman, kItalian, kLangCount };

const wchar_t* LangName(int lang);  // in its own language: "Español", ...
int Detect();                       // Windows' display language, English when it is not one of these
void Set(int lang);
int Get();

// The current language's version of an English string (the string itself when there is none).
const wchar_t* T(const wchar_t* english);
// T() with {0}, {1}, ... replaced by `args`.
std::wstring F(const wchar_t* english, std::initializer_list<std::wstring> args);

}  // namespace i18n
