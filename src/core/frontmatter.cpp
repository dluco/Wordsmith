#include "frontmatter.hpp"

namespace wordsmith::frontmatter {

namespace {

constexpr std::string_view BOM  = "\xEF\xBB\xBF";
constexpr std::size_t      NPOS = std::string_view::npos;

/* One physical line: where it starts, where its content stops (trailing
 * whitespace and any `\r` excluded), and where the next line starts. */
struct LineSpan {
    std::size_t begin       = 0;
    std::size_t content_end = 0;
    std::size_t next        = 0;
};

LineSpan take_line(std::string_view text, std::size_t pos)
{
    const std::size_t newline = text.find('\n', pos);
    const std::size_t stop    = newline == NPOS ? text.size() : newline;

    std::size_t content_end = stop;
    while (content_end > pos
           && (text[content_end - 1] == ' ' || text[content_end - 1] == '\t'
               || text[content_end - 1] == '\r')) {
        --content_end;
    }
    return LineSpan{pos, content_end, newline == NPOS ? text.size() : newline + 1};
}

/* A fence is the whole line, so any indentation at all disqualifies it — an
 * indented `---` is a code block, not a fence. `...` closes but never opens. */
bool is_fence(std::string_view text, const LineSpan& line, bool opening)
{
    const std::string_view content = text.substr(line.begin, line.content_end - line.begin);
    return content == "---" || (!opening && content == "...");
}

/* Leading spaces of the line starting at `pos`. */
int indent_at(std::string_view text, std::size_t pos)
{
    int indent = 0;
    while (pos + static_cast<std::size_t>(indent) < text.size()
           && text[pos + static_cast<std::size_t>(indent)] == ' ') {
        ++indent;
    }
    return indent;
}

} // namespace

Split scan(std::string_view text)
{
    Split split;

    std::size_t start = 0;
    if (text.size() >= BOM.size() && text.compare(0, BOM.size(), BOM) == 0) {
        start = BOM.size();
    }
    /* With no frontmatter the body is everything, the BOM aside — the prologue
     * still holds it, so a round trip keeps it. */
    split.body_begin = start;

    if (start >= text.size()) {
        return split;
    }

    const LineSpan opening = take_line(text, start);
    if (!is_fence(text, opening, /*opening=*/true)) {
        return split;
    }

    for (std::size_t pos = opening.next; pos < text.size();) {
        const LineSpan line = take_line(text, pos);
        if (is_fence(text, line, /*opening=*/false)) {
            split.present    = true;
            split.yaml_begin = opening.next;
            split.yaml_end   = line.begin;
            split.body_begin = line.next;
            return split;
        }
        pos = line.next;
    }

    /* Opened and never closed. Rule 4: this was a thematic break, or a document
     * someone is still in the middle of typing. Either way, not frontmatter. */
    return split;
}

Document parse(std::string_view text)
{
    Document document;
    document.split = scan(text);
    document.root.kind = yaml::Node::Kind::Map;

    if (!document.split.present) {
        return document;
    }

    const std::string_view region =
        text.substr(document.split.yaml_begin,
                    document.split.yaml_end - document.split.yaml_begin);
    document.root =
        yaml::parse(region, document.split.yaml_begin, &document.diagnostics);

    /* The parser numbers lines within the YAML region; the opening fence makes
     * document line numbers one greater. */
    for (yaml::Diagnostic& diagnostic : document.diagnostics) {
        diagnostic.line += 1;
    }
    return document;
}

std::string set_field(std::string_view text, std::string_view key,
                      std::optional<std::string_view> value)
{
    const Document    document = parse(text);
    const yaml::Node* node     = document.root.find(key);

    if (node != nullptr) {
        std::string out(text);
        if (!value) {
            out.erase(node->entry.begin, node->entry.size());
            return out;
        }

        std::string replacement =
            yaml::encode_scalar(*value, indent_at(text, node->entry.begin));
        /* A bare `key:`, and a value that lives on the lines below its key,
         * both leave the range starting flush against the colon — so the
         * separating space has to come from here. */
        if (node->value.begin > 0 && text[node->value.begin - 1] != ' ') {
            replacement.insert(replacement.begin(), ' ');
        }
        out.replace(node->value.begin, node->value.size(), replacement);
        return out;
    }

    if (!value) {
        return std::string(text);
    }

    const std::string entry =
        std::string(key) + ": " + yaml::encode_scalar(*value, 0) + "\n";

    std::string out(text);
    if (document.split.present) {
        /* Append as the last field, just above the closing fence. */
        out.insert(document.split.yaml_end, entry);
    } else {
        out.insert(document.split.body_begin, "---\n" + entry + "---\n");
    }
    return out;
}

} // namespace wordsmith::frontmatter
