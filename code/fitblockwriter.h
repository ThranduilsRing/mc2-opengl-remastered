#pragma once
// FITBLOCK-WRITER-1 — raw brace-block writer for Brain{} / TechSpecial{} blocks.
//
// FitIniFile can only WRITE classic [Section] key=value FIT data — it cannot emit
// the brace-block DSL the declarative Brain{} parser (brain_missionfit_oporbd.cpp)
// and the BrainSpecial raw scanner (brain_special_dispatch.cpp) read. This writer is
// the KEYSTONE for the editable editor Brain panel and the BrainSpecial line editor
// (TECHSCRIPT-GAP-CLOSURE-1 ledger #22; also the cross-lane editor keystone flagged
// in the portfolio recon: "EDITABLE panel needs a raw Brain{} writer").
//
// Design:
//   - Pure std C++ leaf: no engine headers — usable from engine, editor (EditRel),
//     and offline harnesses alike.
//   - Emits the hand-authored carver shape: TAB indentation, `key = "value"` quoted
//     fields, bare-token fields, single-line inline nested blocks
//     (`PrimaryOPORD { type = PlayerControlled }`), and verbatim raw lines for
//     Body DO-verbs / WAIT / STOP / comments.
//   - Deterministic output: same call sequence -> byte-identical text (editor diffs
//     and golden tests stay stable).
//   - Balanced-block safety: saveToFile()/str() usable any time, but saveToFile()
//     refuses (returns false) while blocks are still open.
//
// ZERO engine callers this slice — utility + harness self-test only; stock behavior
// byte-identical by construction. First consumers: EDITOR-BRAIN-PANEL-1 (editable
// panel) and EDITOR-BRAINSPECIAL-LINEEDIT-1.
//
// Proof: tools/brain_dispatch_harness --fitwriter-selftest
//   (writer-emitted TechSpecial text re-parsed by the REAL raw scanner: keys, alias
//    fields, variantOf inheritance, quoted DO args, WAIT/STOP flow verbs round-trip;
//    Brain{} golden-string compare; unbalanced-save negative test.)

#include <string>
#include <vector>

class FitBlockWriter {
public:
    // Open a multi-line block:  <indent>name {
    void beginBlock(const std::string& name);

    // Close the innermost open block:  <indent>}
    // No-op (with a "; FITBLOCK-WRITER underflow" comment line) if nothing is open.
    void endBlock();

    // Single-line nested block:  <indent>name { contents }
    void writeInlineBlock(const std::string& name, const std::string& contents);

    // Quoted string field:  <indent>key = "value"
    void writeString(const std::string& key, const std::string& value);

    // Bare token field (enums, numbers already formatted):  <indent>key = token
    void writeToken(const std::string& key, const std::string& token);

    // Numeric fields.
    void writeLong(const std::string& key, long v);
    void writeFloat(const std::string& key, float v, int precision = 2);
    void writeBool(const std::string& key, bool v);   // key = true / false

    // Comment line:  <indent>; text
    void writeComment(const std::string& text);

    // Verbatim line at the current indent (Body DO-verbs, WAIT/STOP, FITini header).
    void writeRaw(const std::string& lineText);

    void blankLine();

    // Open-block depth (0 = balanced).
    int depth() const { return (int)m_stack.size(); }

    // Accumulated text (valid at any time; callers own balance checking via depth()).
    const std::string& str() const { return m_out; }

    // Write to disk. Returns false on unbalanced blocks (depth() != 0) or IO error.
    bool saveToFile(const char* path) const;

    // Reset to empty (reuse the writer for another file).
    void clear();

private:
    void appendIndent();
    void appendLine(const std::string& s);

    std::string              m_out;
    std::vector<std::string> m_stack;   // open block names (for diagnostics)
};
