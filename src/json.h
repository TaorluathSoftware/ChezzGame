// Minimal JSON reader/escaper: enough for Firebase Auth and Firestore REST replies.
#pragma once
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;

    const Json& operator[](const char* key) const;   // missing keys give a null value
    const Json& operator[](size_t i) const;
    bool has(const char* key) const;
    static Json parse(const std::string& text);
};

std::string jsonEscape(const std::string& s);   // returns a quoted JSON string

// Firestore carries binary fields as base64.
std::string base64Encode(const void* data, size_t n);
std::vector<unsigned char> base64Decode(const std::string& text);
