#pragma once
#include <windows.h>
#include <cwctype>
#include <string>

// UI in Russian or English, by the system UI language (RUG_LANG=ru or en overrides it).
inline bool isRussian() {
    static int ru = -1;
    if (ru < 0) {
        wchar_t v[8] = L"";
        if (GetEnvironmentVariableW(L"RUG_LANG", v, 8) > 0) ru = (v[0] == L'r' && v[1] == L'u') ? 1 : 0;
        else ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? 1 : 0;
    }
    return ru == 1;
}

inline const wchar_t* tr(const wchar_t* ru, const wchar_t* en) { return isRussian() ? ru : en; }

// Captions of the built-in rugs and floors, by file name without the number.
inline const wchar_t* builtinName(const std::wstring& n) {
    struct Entry { const wchar_t *key, *ru, *en; };
    static const Entry names[] = {
        {L"persian", L"Персидский", L"Persian"},
        {L"neuro", L"Нейроковер", L"Neural"},
        {L"soviet", L"Советский", L"Soviet"},
        {L"pixel", L"Пиксельный", L"Pixel"},
        {L"soviet-herringbone", L"Советская елочка", L"Soviet herringbone"},
        {L"soviet-mosaic", L"Советская мозаика", L"Soviet mosaic"},
        {L"oak-planks", L"Дубовые доски", L"Oak planks"},
    };
    for (const Entry& e : names)
        if (n == e.key) return tr(e.ru, e.en);
    return nullptr;
}

inline std::wstring displayName(const std::wstring& path) {
    std::wstring n = path.substr(path.find_last_of(L"\\/") + 1);
    size_t dot = n.find_last_of(L'.');
    if (dot != std::wstring::npos) n = n.substr(0, dot);
    // "1 persian" -> "persian", the number only sets the order
    size_t sp = n.find(L' ');
    if (sp != std::wstring::npos && sp > 0) {
        bool digits = true;
        for (size_t i = 0; i < sp; i++) digits = digits && iswdigit(n[i]);
        if (digits) n = n.substr(sp + 1);
    }
    if (const wchar_t* b = builtinName(n)) return b;
    return n;
}
