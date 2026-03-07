// cpp/server/regex_ue5_enricher.cpp
#include "regex_ue5_enricher.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>

namespace waxcpp::server {

// ── Utility ──────────────────────────────────────────────────

namespace {

std::string_view TrimAscii(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            lines.emplace_back(text.substr(pos));
            break;
        }
        lines.emplace_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return lines;
}

bool StartsWithWord(std::string_view line, std::string_view word) {
    if (line.size() < word.size()) return false;
    if (line.substr(0, word.size()) != word) return false;
    if (line.size() == word.size()) return true;
    const char ch = line[word.size()];
    return std::isspace(static_cast<unsigned char>(ch)) || ch == '(' || ch == ':' || ch == '{';
}

/// Extract an identifier starting at `pos`, consisting of [A-Za-z0-9_].
std::string ExtractIdent(std::string_view s, std::size_t pos) {
    const std::size_t start = pos;
    while (pos < s.size()) {
        const unsigned char ch = static_cast<unsigned char>(s[pos]);
        if (!(std::isalnum(ch) || ch == '_')) break;
        ++pos;
    }
    return std::string(s.substr(start, pos - start));
}

/// Skip whitespace from pos.
std::size_t SkipWS(std::string_view s, std::size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    return pos;
}

/// Check if identifier looks like a UE API macro: all uppercase + ends with _API.
bool IsApiMacro(std::string_view id) {
    if (id.size() < 5) return false;
    if (!id.ends_with("_API")) return false;
    for (char ch : id) {
        if (!(std::isupper(static_cast<unsigned char>(ch)) || ch == '_' || std::isdigit(static_cast<unsigned char>(ch)))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// ── ExtractBalancedParens ────────────────────────────────────

std::string RegexUe5Enricher::ExtractBalancedParens(
    const std::vector<std::string>& lines,
    std::size_t start_idx,
    std::size_t paren_offset,
    std::size_t* end_line_out) {

    std::string result;
    int depth = 0;
    bool started = false;

    for (std::size_t i = start_idx; i < lines.size(); ++i) {
        const auto& line = lines[i];
        const std::size_t begin = (i == start_idx) ? paren_offset : 0;
        for (std::size_t j = begin; j < line.size(); ++j) {
            const char ch = line[j];
            if (ch == '(') {
                if (!started) {
                    started = true;
                    depth = 1;
                    continue;  // skip the opening paren
                }
                ++depth;
                result += ch;
            } else if (ch == ')') {
                --depth;
                if (depth == 0) {
                    if (end_line_out) *end_line_out = i;
                    return result;
                }
                result += ch;
            } else if (started) {
                result += ch;
            }
        }
        if (started && depth > 0) {
            result += ' ';  // join multi-line with space
        }
    }
    return {};  // unbalanced
}

// ── SplitSpecifiers ──────────────────────────────────────────

std::vector<std::string> RegexUe5Enricher::SplitSpecifiers(std::string_view content) {
    std::vector<std::string> result;
    std::string current;
    int paren_depth = 0;

    for (char ch : content) {
        if (ch == '(') {
            ++paren_depth;
            current += ch;
        } else if (ch == ')') {
            --paren_depth;
            current += ch;
        } else if (ch == ',' && paren_depth == 0) {
            auto trimmed = TrimAscii(current);
            if (!trimmed.empty()) {
                result.emplace_back(trimmed);
            }
            current.clear();
        } else {
            current += ch;
        }
    }
    auto trimmed = TrimAscii(current);
    if (!trimmed.empty()) {
        result.emplace_back(trimmed);
    }
    return result;
}

// ── ExtractCategory ──────────────────────────────────────────

std::string RegexUe5Enricher::ExtractCategory(const std::vector<std::string>& specifiers) {
    for (const auto& spec : specifiers) {
        // Match: Category = "Value" or Category="Value"
        auto sv = std::string_view(spec);
        if (!sv.starts_with("Category")) continue;
        const auto eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        auto val = TrimAscii(sv.substr(eq + 1));
        // Strip quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        return std::string(val);
    }
    return {};
}

// ── JoinSpecifiers ───────────────────────────────────────────

std::string RegexUe5Enricher::JoinSpecifiers(const std::vector<std::string>& specifiers) {
    std::string result;
    for (const auto& s : specifiers) {
        // Skip Category and meta — those get their own facts
        if (s.starts_with("Category") || s.starts_with("meta")) continue;
        if (!result.empty()) result += ',';
        result += s;
    }
    return result;
}

// ── MakeFactMeta ─────────────────────────────────────────────

waxcpp::Metadata RegexUe5Enricher::MakeFactMeta(const Ue5ChunkRecord& record) const {
    return {
        {"enricher_kind", "regex_ue5"},
        {"source_path", record.relative_path},
        {"source_lines", std::to_string(record.line_start) + "-" + std::to_string(record.line_end)},
        {"source_chunk_id", record.chunk_id},
    };
}

// ── ParseClassDecl ───────────────────────────────────────────

void RegexUe5Enricher::ParseClassDecl(
    const std::vector<std::string>& lines,
    std::size_t class_line_idx,
    const std::string& macro_kind,
    const std::vector<std::string>& macro_specifiers,
    const Ue5ChunkRecord& record,
    FactBatch& out,
    std::string& current_class_out) {

    const auto trimmed = TrimAscii(lines[class_line_idx]);
    const auto meta = MakeFactMeta(record);

    // Parse: "class [API_MACRO] ClassName [: public Base1, public Base2]"
    std::size_t pos = 0;

    // Skip "class " or "struct " or "enum " or "enum class "
    for (auto kw : {"enum class ", "enum struct ", "class ", "struct ", "enum "}) {
        if (trimmed.starts_with(kw)) {
            pos = std::string_view(kw).size();
            break;
        }
    }
    pos = SkipWS(trimmed, pos);

    // Possibly API macro
    std::string api_macro;
    const auto first_id = ExtractIdent(trimmed, pos);
    if (IsApiMacro(first_id)) {
        api_macro = first_id;
        pos += first_id.size();
        pos = SkipWS(trimmed, pos);
    }

    // Class name
    const auto class_name = ExtractIdent(trimmed, pos);
    if (class_name.empty()) return;

    const std::string entity = "cpp:" + class_name;
    current_class_out = class_name;

    // Emit kind fact
    out.push_back({entity, "kind", macro_kind, meta});

    // Emit API macro
    if (!api_macro.empty()) {
        out.push_back({entity, "api_macro", api_macro, meta});
    }

    // Emit specifiers
    const auto joined = JoinSpecifiers(macro_specifiers);
    if (!joined.empty()) {
        out.push_back({entity, "specifiers", joined, meta});
    }

    // Parse inheritance: find ":"
    pos += class_name.size();
    const auto colon_pos = trimmed.find(':', pos);
    if (colon_pos != std::string_view::npos) {
        auto inheritance = TrimAscii(trimmed.substr(colon_pos + 1));
        // Remove trailing { if present
        if (auto brace = inheritance.find('{'); brace != std::string_view::npos) {
            inheritance = TrimAscii(inheritance.substr(0, brace));
        }
        // Split by comma, extract base class names
        std::string inh_str(inheritance);
        auto bases = SplitSpecifiers(inh_str);
        for (const auto& base : bases) {
            // Strip "public ", "protected ", "private "
            auto bsv = std::string_view(base);
            for (auto access : {"public ", "protected ", "private "}) {
                if (bsv.starts_with(access)) {
                    bsv = TrimAscii(bsv.substr(std::string_view(access).size()));
                    break;
                }
            }
            auto base_name = ExtractIdent(bsv, 0);
            if (!base_name.empty()) {
                out.push_back({entity, "inherits", base_name, meta});
            }
        }
    }
}

// ── ParsePropertyDecl ────────────────────────────────────────

void RegexUe5Enricher::ParsePropertyDecl(
    const std::vector<std::string>& /*lines*/,
    std::size_t /*decl_line_idx*/,
    const std::vector<std::string>& specifiers,
    const std::string& owning_class,
    const Ue5ChunkRecord& record,
    FactBatch& out) {

    // We receive the declaration line content via the caller
    // For now, the caller passes lines[decl_line_idx]
    // The property parsing is handled inline in Enrich() since we need
    // to scan the next non-empty line after UPROPERTY()

    // This method is called with already-parsed property info
    (void)specifiers;
    (void)owning_class;
    (void)record;
    (void)out;
}

// ── ParseFunctionDecl ────────────────────────────────────────

void RegexUe5Enricher::ParseFunctionDecl(
    const std::vector<std::string>& /*lines*/,
    std::size_t /*decl_line_idx*/,
    const std::vector<std::string>& specifiers,
    const std::string& owning_class,
    const Ue5ChunkRecord& record,
    FactBatch& out) {

    (void)specifiers;
    (void)owning_class;
    (void)record;
    (void)out;
}

// ── ParseInclude ─────────────────────────────────────────────

void RegexUe5Enricher::ParseInclude(
    std::string_view line,
    const Ue5ChunkRecord& record,
    FactBatch& out) {

    const auto trimmed = TrimAscii(line);
    if (!trimmed.starts_with("#include")) return;

    auto rest = trimmed.substr(8);  // skip "#include"
    rest = TrimAscii(rest);

    char open_ch = 0, close_ch = 0;
    if (!rest.empty() && rest.front() == '"') { open_ch = '"'; close_ch = '"'; }
    else if (!rest.empty() && rest.front() == '<') { open_ch = '<'; close_ch = '>'; }
    if (open_ch == 0) return;

    const auto close_pos = rest.find(close_ch, 1);
    if (close_pos == std::string_view::npos) return;

    const auto path = rest.substr(1, close_pos - 1);
    if (path.empty()) return;

    const std::string entity = "file:" + record.relative_path;
    out.push_back({entity, "includes", std::string(path), MakeFactMeta(record)});
}

// ── Main Enrich method ───────────────────────────────────────

FactBatch RegexUe5Enricher::Enrich(
    const Ue5ChunkRecord& record,
    std::string_view chunk_text) {

    FactBatch out;
    const auto lines = SplitLines(chunk_text);
    if (lines.empty()) return out;

    // State machine
    enum State { IDLE, AWAITING_CLASS, AWAITING_PROPERTY_DECL, AWAITING_FUNCTION_DECL };
    State state = IDLE;
    int idle_countdown = 0;  // lines remaining before giving up on match

    std::string macro_kind;  // "uclass", "ustruct", "uenum"
    std::vector<std::string> pending_specifiers;
    std::string current_class = record.symbol;  // fallback owning class

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto trimmed = TrimAscii(lines[i]);
        if (trimmed.empty()) {
            if (state != IDLE && --idle_countdown <= 0) state = IDLE;
            continue;
        }

        // ── #include (always, regardless of state) ──
        if (trimmed.starts_with("#include")) {
            ParseInclude(trimmed, record, out);
            continue;
        }

        // ── UCLASS / USTRUCT / UENUM ──
        if (state == IDLE) {
            bool found_macro = false;
            std::string detected_kind;

            if (trimmed.starts_with("UCLASS(") || trimmed.starts_with("UCLASS (")) {
                detected_kind = "uclass"; found_macro = true;
            } else if (trimmed.starts_with("USTRUCT(") || trimmed.starts_with("USTRUCT (")) {
                detected_kind = "ustruct"; found_macro = true;
            } else if (trimmed.starts_with("UENUM(") || trimmed.starts_with("UENUM (")) {
                detected_kind = "uenum"; found_macro = true;
            }

            if (found_macro) {
                const auto paren_pos = trimmed.find('(');
                std::size_t end_line = i;
                const auto paren_content = ExtractBalancedParens(lines, i, paren_pos, &end_line);
                pending_specifiers = SplitSpecifiers(paren_content);
                macro_kind = detected_kind;
                state = AWAITING_CLASS;
                idle_countdown = 5;  // allow up to 5 lines gap
                i = end_line;  // skip to end of macro parens
                continue;
            }

            // ── UPROPERTY ──
            if (trimmed.starts_with("UPROPERTY(") || trimmed.starts_with("UPROPERTY (")) {
                const auto paren_pos = trimmed.find('(');
                std::size_t end_line = i;
                const auto paren_content = ExtractBalancedParens(lines, i, paren_pos, &end_line);
                pending_specifiers = SplitSpecifiers(paren_content);
                state = AWAITING_PROPERTY_DECL;
                idle_countdown = 3;
                i = end_line;
                continue;
            }

            // ── UFUNCTION ──
            if (trimmed.starts_with("UFUNCTION(") || trimmed.starts_with("UFUNCTION (")) {
                const auto paren_pos = trimmed.find('(');
                std::size_t end_line = i;
                const auto paren_content = ExtractBalancedParens(lines, i, paren_pos, &end_line);
                pending_specifiers = SplitSpecifiers(paren_content);
                state = AWAITING_FUNCTION_DECL;
                idle_countdown = 3;
                i = end_line;
                continue;
            }
        }

        // ── State: AWAITING_CLASS ──
        if (state == AWAITING_CLASS) {
            if (StartsWithWord(trimmed, "class") ||
                StartsWithWord(trimmed, "struct") ||
                StartsWithWord(trimmed, "enum")) {
                ParseClassDecl(lines, i, macro_kind, pending_specifiers, record, out, current_class);
                state = IDLE;
                continue;
            }
            if (--idle_countdown <= 0) state = IDLE;
        }

        // ── State: AWAITING_PROPERTY_DECL ──
        if (state == AWAITING_PROPERTY_DECL) {
            // Parse: "TYPE NAME;" or "TYPE NAME = ...;"
            // Skip lines that look like more macros or comments
            if (trimmed.starts_with("//") || trimmed.starts_with("/*") ||
                trimmed.starts_with("UPROPERTY") || trimmed.starts_with("UFUNCTION") ||
                trimmed.starts_with("GENERATED")) {
                if (--idle_countdown <= 0) state = IDLE;
                continue;
            }

            // Find the last identifier before ; or = or end
            auto decl = std::string(trimmed);
            // Remove trailing ; and anything after =
            auto semicolon = decl.find(';');
            if (semicolon != std::string::npos) decl = decl.substr(0, semicolon);
            auto eq = decl.find('=');
            if (eq != std::string::npos) decl = decl.substr(0, eq);
            auto trimmed_decl = TrimAscii(decl);

            // Extract property name (last identifier)
            std::size_t name_end = trimmed_decl.size();
            while (name_end > 0 && std::isspace(static_cast<unsigned char>(trimmed_decl[name_end - 1]))) --name_end;
            std::size_t name_start = name_end;
            while (name_start > 0) {
                const unsigned char ch = static_cast<unsigned char>(trimmed_decl[name_start - 1]);
                if (!(std::isalnum(ch) || ch == '_')) break;
                --name_start;
            }

            if (name_start < name_end) {
                const auto prop_name = std::string(trimmed_decl.substr(name_start, name_end - name_start));
                const auto type_part = TrimAscii(trimmed_decl.substr(0, name_start));

                const std::string entity = "cpp:" + current_class + "." + prop_name;
                const auto meta = MakeFactMeta(record);

                if (!type_part.empty()) {
                    out.push_back({entity, "type", std::string(type_part), meta});
                }

                const auto joined = JoinSpecifiers(pending_specifiers);
                if (!joined.empty()) {
                    out.push_back({entity, "specifiers", joined, meta});
                }

                const auto category = ExtractCategory(pending_specifiers);
                if (!category.empty()) {
                    out.push_back({entity, "category", category, meta});
                }

                out.push_back({entity, "kind", "uproperty", meta});
            }

            state = IDLE;
            continue;
        }

        // ── State: AWAITING_FUNCTION_DECL ──
        if (state == AWAITING_FUNCTION_DECL) {
            if (trimmed.starts_with("//") || trimmed.starts_with("/*") ||
                trimmed.starts_with("UPROPERTY") || trimmed.starts_with("UFUNCTION") ||
                trimmed.starts_with("GENERATED")) {
                if (--idle_countdown <= 0) state = IDLE;
                continue;
            }

            // Parse: "ReturnType FuncName(params)" or "virtual ReturnType FuncName(..."
            auto sv = trimmed;

            // Strip "virtual " and "static " prefixes
            if (sv.starts_with("virtual ")) sv = TrimAscii(sv.substr(8));
            if (sv.starts_with("static ")) sv = TrimAscii(sv.substr(7));

            // Find the opening paren of the function signature
            const auto open_paren = sv.find('(');
            if (open_paren == std::string_view::npos || open_paren == 0) {
                if (--idle_countdown <= 0) state = IDLE;
                continue;
            }

            // Function name is the identifier just before '('
            std::size_t fname_end = open_paren;
            while (fname_end > 0 && std::isspace(static_cast<unsigned char>(sv[fname_end - 1]))) --fname_end;
            std::size_t fname_start = fname_end;
            while (fname_start > 0) {
                const unsigned char ch = static_cast<unsigned char>(sv[fname_start - 1]);
                if (!(std::isalnum(ch) || ch == '_')) break;
                --fname_start;
            }

            if (fname_start >= fname_end) {
                state = IDLE;
                continue;
            }

            const auto func_name = std::string(sv.substr(fname_start, fname_end - fname_start));
            const auto ret_type = TrimAscii(sv.substr(0, fname_start));

            const std::string entity = "cpp:" + current_class + "::" + func_name;
            const auto meta = MakeFactMeta(record);

            if (!ret_type.empty()) {
                out.push_back({entity, "returns", std::string(ret_type), meta});
            }

            const auto joined = JoinSpecifiers(pending_specifiers);
            if (!joined.empty()) {
                out.push_back({entity, "specifiers", joined, meta});
            }

            const auto category = ExtractCategory(pending_specifiers);
            if (!category.empty()) {
                out.push_back({entity, "category", category, meta});
            }

            out.push_back({entity, "kind", "ufunction", meta});

            state = IDLE;
            continue;
        }
    }

    return out;
}

}  // namespace waxcpp::server
