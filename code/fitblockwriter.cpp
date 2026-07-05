// FITBLOCK-WRITER-1 — raw brace-block writer implementation.
// Pure std C++ leaf (no engine headers). See fitblockwriter.h for the contract.

#include "fitblockwriter.h"

#include <cstdio>
#include <fstream>

void FitBlockWriter::appendIndent() {
    for (size_t i = 0; i < m_stack.size(); ++i)
        m_out += '\t';
}

void FitBlockWriter::appendLine(const std::string& s) {
    appendIndent();
    m_out += s;
    m_out += '\n';
}

void FitBlockWriter::beginBlock(const std::string& name) {
    appendLine(name + " {");
    m_stack.push_back(name);
}

void FitBlockWriter::endBlock() {
    if (m_stack.empty()) {
        // Underflow — leave a visible marker instead of corrupting the output.
        appendLine("; FITBLOCK-WRITER underflow: endBlock() with no open block");
        return;
    }
    m_stack.pop_back();
    appendLine("}");
}

void FitBlockWriter::writeInlineBlock(const std::string& name, const std::string& contents) {
    appendLine(name + " { " + contents + " }");
}

void FitBlockWriter::writeString(const std::string& key, const std::string& value) {
    appendLine(key + " = \"" + value + "\"");
}

void FitBlockWriter::writeToken(const std::string& key, const std::string& token) {
    appendLine(key + " = " + token);
}

void FitBlockWriter::writeLong(const std::string& key, long v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%ld", v);
    writeToken(key, buf);
}

void FitBlockWriter::writeFloat(const std::string& key, float v, int precision) {
    if (precision < 0)  precision = 0;
    if (precision > 9)  precision = 9;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
    writeToken(key, buf);
}

void FitBlockWriter::writeBool(const std::string& key, bool v) {
    writeToken(key, v ? "true" : "false");
}

void FitBlockWriter::writeComment(const std::string& text) {
    appendLine("; " + text);
}

void FitBlockWriter::writeRaw(const std::string& lineText) {
    appendLine(lineText);
}

void FitBlockWriter::blankLine() {
    m_out += '\n';
}

bool FitBlockWriter::saveToFile(const char* path) const {
    if (!path || path[0] == '\0') return false;
    if (!m_stack.empty()) return false;   // unbalanced — refuse to persist
    std::ofstream f(path, std::ios::binary);   // binary: deterministic \n endings
    if (!f.is_open()) return false;
    f.write(m_out.data(), (std::streamsize)m_out.size());
    return f.good();
}

void FitBlockWriter::clear() {
    m_out.clear();
    m_stack.clear();
}
