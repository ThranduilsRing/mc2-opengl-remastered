// tools/gate_dump/main.cpp -- print the compiled gate table as JSON (LF, one gate per line).
//
// The output is committed as Gates/gate_table.json. tools/gate_gen.py derives the generated
// index from it and `gate_gen.py --check --rebuild` re-runs this tool and diffs the result,
// so the C++ table is never parsed by anything but the compiler.
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "gate_row.h"

using mc2::gate::Kind;
using mc2::gate::Row;
using mc2::gate::Rule;

namespace {

const char* kindId(Kind k) {
    switch (k) {
        case Kind::Bool:   return "Bool";
        case Kind::Tri:    return "Tri";
        case Kind::Int:    return "Int";
        case Kind::Float:  return "Float";
        case Kind::String: return "String";
        case Kind::Enum:   return "Enum";
    }
    return "?";
}

const char* ruleId(Rule r) {
    switch (r) {
        case Rule::Std:         return "Std";
        case Rule::Presence:    return "Presence";
        case Rule::NonEmpty:    return "NonEmpty";
        case Rule::Exact1:      return "Exact1";
        case Rule::Lead1:       return "Lead1";
        case Rule::Lead0:       return "Lead0";
        case Rule::Lead0Kill:   return "Lead0Kill";
        case Rule::Lead0P:      return "Lead0P";
        case Rule::DefOnLead0:  return "DefOnLead0";
        case Rule::AtoiNZ:      return "AtoiNZ";
        case Rule::DefOnAtoiNZ: return "DefOnAtoiNZ";
        case Rule::Lead0Def:    return "Lead0Def";
        case Rule::Is01:        return "Is01";
        case Rule::Prefix:      return "Prefix";
    }
    return "?";
}

void str(std::string& out, std::string_view s) {
    out += '"';
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else out += static_cast<char>(c);
    }
    out += '"';
}

void num(std::string& out, double v, const char* fmt) {
    char b[48];
    std::snprintf(b, sizeof b, fmt, v);
    out += b;
}

}  // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // the JSON is LF on every host
#endif
    const Row* t = mc2::gate::tableData();
    const std::size_t n = mc2::gate::tableSize();
    std::string out;
    out += "{\n \"version\": 1,\n \"count\": ";
    out += std::to_string(n);
    out += ",\n \"gates\": [\n";
    for (std::size_t i = 0; i < n; ++i) {
        const Row& r = t[i];
        out += "  {\"name\": ";
        str(out, r.name);
        out += ", \"kind\": \"";
        out += kindId(r.kind);
        out += "\", \"rule\": \"";
        out += ruleId(r.rule);
        out += "\", \"default\": ";
        switch (r.kind) {
            case Kind::Bool:   out += r.defBool ? "true" : "false"; break;
            case Kind::Tri:    out += "null"; break;
            case Kind::Int:    out += std::to_string(r.defInt); break;
            case Kind::Float:  num(out, static_cast<double>(r.defFloat), "%.9g"); break;
            case Kind::String:
            case Kind::Enum:   str(out, r.defStr); break;
        }
        out += ", \"owner\": ";
        str(out, r.owner);
        out += ", \"retireBy\": ";
        str(out, r.retireBy);
        out += ", \"doc\": ";
        str(out, r.doc);
        out += ", \"tokens\": ";
        str(out, r.tokens);
        out += ", \"range\": ";
        if (r.range.has) {
            out += "{\"lo\": ";
            num(out, r.range.lo, "%.17g");
            out += ", \"hi\": ";
            num(out, r.range.hi, "%.17g");
            out += ", \"clamp\": ";
            out += r.range.clamp ? "true" : "false";
            if (r.range.loExcl) out += ", \"loExclusive\": true";
            out += "}";
        } else out += "null";
        out += ", \"reread\": ";
        out += r.reread ? "true" : "false";
        out += ", \"divergent\": ";
        out += r.divergent ? "true" : "false";
        out += ", \"smokePassthrough\": ";
        out += r.smokePassthrough ? "true" : "false";
        out += ", \"logTopic\": ";
        str(out, r.logTopic);
        out += "}";
        out += (i + 1 < n) ? ",\n" : "\n";
    }
    out += " ]\n}\n";
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
