#include "json.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const Json NULL_JSON;

const Json& Json::operator[](const char* key) const {
    for (auto& kv : o)
        if (kv.first == key) return kv.second;
    return NULL_JSON;
}
const Json& Json::operator[](size_t i) const { return i < a.size() ? a[i] : NULL_JSON; }
bool Json::has(const char* key) const {
    for (auto& kv : o)
        if (kv.first == key) return true;
    return false;
}

namespace {
struct Parser {
    const char* p;
    const char* end;
    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
    }
    std::string str() {
        std::string out;
        p++;   // opening quote
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                p++;
                switch (*p) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u':
                        if (p + 4 < end) {
                            char hex[5] = {p[1], p[2], p[3], p[4], 0};
                            putUtf8(out, (unsigned)strtoul(hex, nullptr, 16));
                            p += 4;
                        }
                        break;
                    default: out += *p;
                }
                p++;
            } else {
                out += *p++;
            }
        }
        if (p < end) p++;
        return out;
    }
    Json value(int depth) {
        Json j;
        ws();
        if (p >= end || depth > 64) return j;
        char c = *p;
        if (c == '{') {
            j.t = Json::OBJ;
            p++;
            ws();
            if (p < end && *p == '}') { p++; return j; }
            while (p < end) {
                ws();
                if (p >= end || *p != '"') break;
                std::string k = str();
                ws();
                if (p < end && *p == ':') p++;
                j.o.emplace_back(k, value(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') p++;
                break;
            }
        } else if (c == '[') {
            j.t = Json::ARR;
            p++;
            ws();
            if (p < end && *p == ']') { p++; return j; }
            while (p < end) {
                j.a.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') p++;
                break;
            }
        } else if (c == '"') {
            j.t = Json::STR;
            j.s = str();
        } else if (c == 't' || c == 'f') {
            j.t = Json::BOOL;
            j.b = c == 't';
            p += j.b ? 4 : 5;
        } else if (c == 'n') {
            p += 4;
        } else {
            j.t = Json::NUM;
            char* e = nullptr;
            j.n = strtod(p, &e);
            p = e && e > p ? e : p + 1;
        }
        return j;
    }
};
}   // namespace

Json Json::parse(const std::string& text) {
    Parser ps{text.data(), text.data() + text.size()};
    return ps.value(0);
}

std::string jsonEscape(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 32) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += (char)c;
    }
    return out + "\"";
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out += i + 2 < n ? B64[v & 63] : '=';
    }
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& text) {
    static int back[256];
    static bool ready = false;
    if (!ready) {
        ready = true;
        for (int i = 0; i < 256; i++) back[i] = -1;
        for (int i = 0; i < 64; i++) back[(unsigned char)B64[i]] = i;
    }
    std::vector<unsigned char> out;
    out.reserve(text.size() / 4 * 3);
    unsigned v = 0;
    int bits = 0;
    for (unsigned char ch : text) {
        int d = back[ch];
        if (d < 0) continue;   // newlines, padding and anything else are skipped
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char)((v >> bits) & 0xFF));
        }
    }
    return out;
}
