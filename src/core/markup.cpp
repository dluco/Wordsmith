//
// Adapted from Ogden's src/core/markup.cpp. The block and inline parsers are
// carried over largely unchanged; underline (<u>) and the serializer are new.
//

#include "markup.hpp"

#include <cctype>

namespace wordsmith::markup {

namespace {

/* ── line helpers ────────────────────────────────────────────────────────── */

std::size_t indent_of(std::string_view line)
{
    std::size_t index = 0;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
        ++index;
    }
    return index;
}

std::string_view lstrip(std::string_view text) { return text.substr(indent_of(text)); }

std::string_view rstrip(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(0, end);
}

std::string_view strip(std::string_view text) { return rstrip(lstrip(text)); }

bool is_blank(std::string_view text) { return strip(text).empty(); }

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_alnum(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; }

/* ── inline markup ───────────────────────────────────────────────────────── */

struct Style {
    bool        emphasis = false;
    bool        strong = false;
    bool        underline = false;
    bool        code = false;
    std::string href;
};

void emit(std::vector<Span>& out, std::string& accumulator, const Style& style)
{
    if (accumulator.empty()) {
        return;
    }
    Span span;
    span.text      = accumulator;
    span.emphasis  = style.emphasis;
    span.strong    = style.strong;
    span.underline = style.underline;
    span.code      = style.code;
    span.href      = style.href;
    out.push_back(std::move(span));
    accumulator.clear();
}

void parse_runs(std::string_view text, Style style, std::vector<Span>& out)
{
    const std::size_t length = text.size();
    std::string accumulator;
    std::size_t index = 0;

    while (index < length) {
        const char ch = text[index];

        // Backslash escape of ASCII punctuation.
        if (ch == '\\' && index + 1 < length
            && std::ispunct(static_cast<unsigned char>(text[index + 1]))) {
            accumulator += text[index + 1];
            index += 2;
            continue;
        }

        // `code` (run of N backticks closed by a run of exactly N).
        if (ch == '`') {
            std::size_t after_open = index;
            while (after_open < length && text[after_open] == '`') {
                ++after_open;
            }
            const std::size_t run = after_open - index;
            std::size_t scan = after_open;
            std::size_t close = std::string_view::npos;
            while (scan < length) {
                if (text[scan] == '`') {
                    std::size_t after = scan;
                    while (after < length && text[after] == '`') {
                        ++after;
                    }
                    if (after - scan == run) {
                        close = scan;
                        break;
                    }
                    scan = after;
                } else {
                    ++scan;
                }
            }
            if (close != std::string_view::npos) {
                emit(out, accumulator, style);
                std::string_view content = text.substr(after_open, close - after_open);
                // CommonMark: strip one surrounding space when both sides have one.
                if (content.size() >= 2 && content.front() == ' '
                    && content.back() == ' ' && strip(content).size() != 0) {
                    content = content.substr(1, content.size() - 2);
                }
                Span span;
                span.text      = std::string(content);
                span.code      = true;
                span.emphasis  = style.emphasis;
                span.strong    = style.strong;
                span.underline = style.underline;
                span.href      = style.href;
                out.push_back(std::move(span));
                index = close + run;
                continue;
            }
            accumulator.append(text.substr(index, run));
            index = after_open;
            continue;
        }

        // [text](url) link.
        if (ch == '[') {
            const std::size_t bracket = text.find(']', index + 1);
            if (bracket != std::string_view::npos && bracket + 1 < length
                && text[bracket + 1] == '(') {
                const std::size_t paren = text.find(')', bracket + 2);
                if (paren != std::string_view::npos) {
                    emit(out, accumulator, style);
                    Style link_style = style;
                    link_style.href =
                        std::string(text.substr(bracket + 2, paren - (bracket + 2)));
                    parse_runs(text.substr(index + 1, bracket - (index + 1)),
                               link_style, out);
                    index = paren + 1;
                    continue;
                }
            }
            accumulator += ch;
            ++index;
            continue;
        }

        // <u>underline</u>. Checked before the autolink branch, which also
        // opens on '<'.
        if (ch == '<' && starts_with(text.substr(index), "<u>")) {
            const std::size_t close = text.find("</u>", index + 3);
            if (close != std::string_view::npos) {
                emit(out, accumulator, style);
                Style underline_style = style;
                underline_style.underline = true;
                parse_runs(text.substr(index + 3, close - (index + 3)),
                           underline_style, out);
                index = close + 4;
                continue;
            }
        }

        // <autolink>
        if (ch == '<') {
            const std::size_t close = text.find('>', index + 1);
            if (close != std::string_view::npos) {
                std::string_view inner = text.substr(index + 1, close - (index + 1));
                if (inner.find("://") != std::string_view::npos) {
                    emit(out, accumulator, style);
                    Span span;
                    span.text      = std::string(inner);
                    span.href      = std::string(inner);
                    span.emphasis  = style.emphasis;
                    span.strong    = style.strong;
                    span.underline = style.underline;
                    out.push_back(std::move(span));
                    index = close + 1;
                    continue;
                }
            }
            accumulator += ch;
            ++index;
            continue;
        }

        // **strong** / __strong__
        if ((ch == '*' || ch == '_') && index + 1 < length && text[index + 1] == ch) {
            const bool underscore = (ch == '_');
            const bool can_open = !underscore || (index == 0 || !is_alnum(text[index - 1]));
            if (can_open) {
                std::size_t scan = index + 2;
                std::size_t close = std::string_view::npos;
                while (scan + 1 < length) {
                    if (text[scan] == ch && text[scan + 1] == ch && scan > index + 2) {
                        const bool can_close =
                            !underscore || (scan + 2 >= length || !is_alnum(text[scan + 2]));
                        if (can_close) {
                            close = scan;
                            break;
                        }
                    }
                    ++scan;
                }
                if (close != std::string_view::npos) {
                    emit(out, accumulator, style);
                    Style strong_style = style;
                    strong_style.strong = true;
                    parse_runs(text.substr(index + 2, close - (index + 2)),
                               strong_style, out);
                    index = close + 2;
                    continue;
                }
            }
        }

        // *emphasis* / _emphasis_
        if (ch == '*' || ch == '_') {
            const bool underscore = (ch == '_');
            const bool can_open = (!underscore || index == 0 || !is_alnum(text[index - 1]))
                && index + 1 < length
                && !std::isspace(static_cast<unsigned char>(text[index + 1]));
            if (can_open) {
                std::size_t scan = index + 1;
                std::size_t close = std::string_view::npos;
                while (scan < length) {
                    if (text[scan] == ch && scan > index + 1
                        && !std::isspace(static_cast<unsigned char>(text[scan - 1]))) {
                        const bool can_close =
                            !underscore || (scan + 1 >= length || !is_alnum(text[scan + 1]));
                        if (can_close) {
                            close = scan;
                            break;
                        }
                    }
                    ++scan;
                }
                if (close != std::string_view::npos) {
                    emit(out, accumulator, style);
                    Style emphasis_style = style;
                    emphasis_style.emphasis = true;
                    parse_runs(text.substr(index + 1, close - (index + 1)),
                               emphasis_style, out);
                    index = close + 1;
                    continue;
                }
            }
        }

        accumulator += ch;
        ++index;
    }
    emit(out, accumulator, style);
}

/* ── block markers ───────────────────────────────────────────────────────── */

bool is_fence(std::string_view stripped, char& fence_char, std::size_t& fence_length)
{
    if (stripped.size() < 3) {
        return false;
    }
    const char ch = stripped[0];
    if (ch != '`' && ch != '~') {
        return false;
    }
    std::size_t count = 0;
    while (count < stripped.size() && stripped[count] == ch) {
        ++count;
    }
    if (count < 3) {
        return false;
    }
    fence_char   = ch;
    fence_length = count;
    return true;
}

bool is_rule(std::string_view stripped)
{
    if (stripped.empty()) {
        return false;
    }
    const char ch = stripped[0];
    if (ch != '-' && ch != '*' && ch != '_') {
        return false;
    }
    std::size_t count = 0;
    for (char current : stripped) {
        if (current == ' ' || current == '\t') {
            continue;
        }
        if (current != ch) {
            return false;
        }
        ++count;
    }
    return count >= 3;
}

// ATX heading: 1-6 '#', then a space or end-of-line.
bool is_heading(std::string_view stripped, int& level, std::string_view& text)
{
    std::size_t hashes = 0;
    while (hashes < stripped.size() && stripped[hashes] == '#') {
        ++hashes;
    }
    if (hashes == 0 || hashes > 6) {
        return false;
    }
    if (hashes < stripped.size() && stripped[hashes] != ' ') {
        return false;
    }
    level = static_cast<int>(hashes);
    std::string_view rest = stripped.substr(hashes);
    // Strip the leading space and any trailing '#'s.
    rest = wordsmith::markup::strip(rest);
    while (!rest.empty() && rest.back() == '#') {
        rest.remove_suffix(1);
    }
    text = wordsmith::markup::rstrip(rest);
    return true;
}

bool is_list_marker(std::string_view stripped, bool& ordered, int& number,
                    std::size_t& marker_len)
{
    if (stripped.size() >= 2
        && (stripped[0] == '-' || stripped[0] == '*' || stripped[0] == '+')
        && stripped[1] == ' ') {
        ordered    = false;
        number     = 0;
        marker_len = 2;
        return true;
    }
    std::size_t digits = 0;
    while (digits < stripped.size()
           && std::isdigit(static_cast<unsigned char>(stripped[digits]))) {
        ++digits;
    }
    if (digits > 0 && digits + 1 < stripped.size()
        && (stripped[digits] == '.' || stripped[digits] == ')')
        && stripped[digits + 1] == ' ') {
        ordered    = true;
        number     = std::stoi(std::string(stripped.substr(0, digits)));
        marker_len = digits + 2;
        return true;
    }
    return false;
}

/* ── block parser ────────────────────────────────────────────────────────── */

class Parser {
public:
    explicit Parser(std::string_view text) { split_lines(text); }

    Document run()
    {
        Document doc;
        while (line_index_ < lines_.size()) {
            if (is_blank(lines_[line_index_])) {
                ++line_index_;
                continue;
            }

            std::string_view stripped = lstrip(lines_[line_index_]);
            char fence_char;
            std::size_t fence_length;
            if (is_fence(stripped, fence_char, fence_length)) {
                parse_fence(doc, fence_char, fence_length);
                continue;
            }
            if (is_rule(stripped)) {
                doc.blocks.push_back(Block{BlockKind::Rule});
                ++line_index_;
                continue;
            }

            int level;
            std::string_view heading_text;
            if (is_heading(stripped, level, heading_text)) {
                Block block;
                block.kind  = BlockKind::Heading;
                block.level = level;
                block.spans = parse_inline(heading_text);
                doc.blocks.push_back(std::move(block));
                ++line_index_;
                continue;
            }
            if (starts_with(stripped, ">")) {
                parse_quote(doc);
                continue;
            }

            bool ordered;
            int number;
            std::size_t marker_len;
            if (is_list_marker(stripped, ordered, number, marker_len)) {
                parse_list(doc);
                continue;
            }

            parse_paragraph(doc);
        }
        return doc;
    }

private:
    void split_lines(std::string_view text)
    {
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t newline = text.find('\n', start);
            if (newline == std::string_view::npos) {
                newline = text.size();
            }
            std::string_view line = text.substr(start, newline - start);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            lines_.emplace_back(line);
            if (newline == text.size()) {
                break;
            }
            start = newline + 1;
        }
    }

    void parse_fence(Document& doc, char fence_char, std::size_t fence_length)
    {
        const std::size_t base = indent_of(lines_[line_index_]);
        std::string_view open = lstrip(lines_[line_index_]);
        std::string language(strip(open.substr(fence_length)));
        ++line_index_;

        std::string body;
        bool first = true;
        while (line_index_ < lines_.size()) {
            std::string_view stripped = lstrip(lines_[line_index_]);
            // Closing fence: at least fence_length of the same char, nothing else.
            if (stripped.size() >= fence_length) {
                std::size_t count = 0;
                while (count < stripped.size() && stripped[count] == fence_char) {
                    ++count;
                }
                if (count >= fence_length && strip(stripped.substr(count)).empty()) {
                    ++line_index_;
                    break;
                }
            }
            // Body line, dedented by the opening indent (leading whitespace only).
            std::string_view line = lines_[line_index_];
            std::size_t cut = line.size() < base ? line.size() : base;
            std::size_t width = 0;
            while (width < cut && (line[width] == ' ' || line[width] == '\t')) {
                ++width;
            }
            if (!first) {
                body += '\n';
            }
            body += std::string(line.substr(width));
            first = false;
            ++line_index_;
        }
        Block block;
        block.kind     = BlockKind::CodeBlock;
        block.language = std::move(language);
        block.code     = std::move(body);
        doc.blocks.push_back(std::move(block));
    }

    void parse_quote(Document& doc)
    {
        std::string text;
        while (line_index_ < lines_.size() && !is_blank(lines_[line_index_])
               && starts_with(lstrip(lines_[line_index_]), ">")) {
            std::string_view inner = lstrip(lines_[line_index_]).substr(1);
            if (!inner.empty() && inner.front() == ' ') {
                inner.remove_prefix(1);
            }
            if (!text.empty()) {
                text += ' ';
            }
            text += std::string(strip(inner));
            ++line_index_;
        }
        Block block;
        block.kind  = BlockKind::Quote;
        block.spans = parse_inline(text);
        doc.blocks.push_back(std::move(block));
    }

    void parse_paragraph(Document& doc)
    {
        std::string text;
        while (line_index_ < lines_.size()) {
            std::string_view line = lines_[line_index_];
            if (is_blank(line)) {
                break;
            }
            std::string_view stripped = lstrip(line);
            char fence_char;
            std::size_t fence_length;
            int level;
            std::string_view heading_text;
            bool ordered;
            int number;
            std::size_t marker_len;
            if (is_fence(stripped, fence_char, fence_length) || is_rule(stripped)
                || is_heading(stripped, level, heading_text)
                || starts_with(stripped, ">")
                || is_list_marker(stripped, ordered, number, marker_len)) {
                break;
            }
            if (!text.empty()) {
                text += ' ';
            }
            text += std::string(strip(line));
            ++line_index_;
        }
        if (!text.empty()) {
            Block block;
            block.kind  = BlockKind::Paragraph;
            block.spans = parse_inline(text);
            doc.blocks.push_back(std::move(block));
        }
    }

    // True if the list region should continue past blank line(s) at `from`.
    bool region_continues(std::size_t from) const
    {
        std::size_t scan = from;
        while (scan < lines_.size() && is_blank(lines_[scan])) {
            ++scan;
        }
        if (scan >= lines_.size()) {
            return false;
        }
        bool ordered;
        int number;
        std::size_t marker_len;
        return indent_of(lines_[scan]) > 0
            || is_list_marker(lstrip(lines_[scan]), ordered, number, marker_len);
    }

    void parse_list(Document& doc)
    {
        std::vector<Block>       items;
        std::vector<std::string> texts;
        std::vector<std::size_t> indents;   // marker indents, deepest last

        while (line_index_ < lines_.size()) {
            if (is_blank(lines_[line_index_])) {
                if (region_continues(line_index_ + 1)) {
                    ++line_index_;
                    continue;
                }
                break;
            }
            std::string_view line = lines_[line_index_];
            const std::size_t indent = indent_of(line);
            std::string_view stripped = lstrip(line);

            bool ordered;
            int number;
            std::size_t marker_len;
            if (is_list_marker(stripped, ordered, number, marker_len)) {
                while (!indents.empty() && indent < indents.back()) {
                    indents.pop_back();
                }
                if (indents.empty() || indent > indents.back()) {
                    indents.push_back(indent);
                }
                Block block;
                block.kind        = BlockKind::ListItem;
                block.level       = static_cast<int>(indents.size()) - 1;
                block.ordered     = ordered;
                block.list_number = number;
                items.push_back(std::move(block));
                texts.emplace_back(strip(stripped.substr(marker_len)));
                ++line_index_;
            } else if (indent > 0 && !items.empty()) {
                // Continuation of the current item.
                if (!texts.back().empty()) {
                    texts.back() += ' ';
                }
                texts.back() += std::string(strip(line));
                ++line_index_;
            } else {
                break;
            }
        }

        for (std::size_t index = 0; index < items.size(); ++index) {
            items[index].spans = parse_inline(texts[index]);
            doc.blocks.push_back(std::move(items[index]));
        }
    }

    std::vector<std::string> lines_;
    std::size_t              line_index_ = 0;
};

/* ── serializer ──────────────────────────────────────────────────────────── */

/* Backslash-escape the characters that would otherwise re-parse as markup.
 * Deliberately conservative: prose is full of apostrophes, dashes and
 * parentheses, and escaping those would make the files unpleasant to read. */
std::string escape_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\\' || ch == '*' || ch == '_' || ch == '`' || ch == '[' || ch == ']') {
            out += '\\';
            out += ch;
            continue;
        }
        // '<' only needs escaping when it would open a tag or an autolink.
        if (ch == '<') {
            out += "\\<";
            continue;
        }
        // A '#' or '>' opening a line would become a heading or a quote.
        if ((ch == '#' || ch == '>') && index == 0) {
            out += '\\';
            out += ch;
            continue;
        }
        out += ch;
    }
    return out;
}

/* Wrap `body` in the delimiters for one span's styles. Order is outermost
 * first: link, then strong, emphasis, underline, with code innermost so its
 * backticks sit directly against the text. */
std::string apply_styles(const Span& span, std::string body)
{
    if (span.code) {
        body = "`" + body + "`";
    }
    if (span.underline) {
        body = "<u>" + body + "</u>";
    }
    if (span.emphasis) {
        body = "*" + body + "*";
    }
    if (span.strong) {
        body = "**" + body + "**";
    }
    if (!span.href.empty()) {
        body = "[" + body + "](" + span.href + ")";
    }
    return body;
}

std::string serialize_block(const Block& block)
{
    switch (block.kind) {
    case BlockKind::Rule:
        return "---";

    case BlockKind::CodeBlock: {
        std::string out = "```";
        out += block.language;
        out += '\n';
        out += block.code;
        if (!block.code.empty() && block.code.back() != '\n') {
            out += '\n';
        }
        out += "```";
        return out;
    }

    case BlockKind::Heading: {
        const int level = block.level < 1 ? 1 : (block.level > 6 ? 6 : block.level);
        std::string out(static_cast<std::size_t>(level), '#');
        out += ' ';
        out += serialize_inline(block.spans);
        return out;
    }

    case BlockKind::Quote:
        return "> " + serialize_inline(block.spans);

    case BlockKind::ListItem: {
        const int depth = block.level < 0 ? 0 : block.level;
        std::string out(static_cast<std::size_t>(depth) * 2, ' ');
        if (block.ordered) {
            out += std::to_string(block.list_number > 0 ? block.list_number : 1);
            out += ". ";
        } else {
            out += "- ";
        }
        out += serialize_inline(block.spans);
        return out;
    }

    case BlockKind::Paragraph:
        break;
    }
    return serialize_inline(block.spans);
}

} // namespace

std::vector<Span> parse_inline(std::string_view text)
{
    std::vector<Span> out;
    parse_runs(text, Style{}, out);
    return out;
}

Document parse(std::string_view markdown)
{
    return Parser(markdown).run();
}

std::string serialize_inline(const std::vector<Span>& spans)
{
    std::string out;
    for (const Span& span : spans) {
        // Code spans are verbatim; escaping inside them would show up literally.
        std::string body = span.code ? span.text : escape_text(span.text);
        out += apply_styles(span, std::move(body));
    }
    return out;
}

std::string serialize(const Document& document)
{
    std::string out;
    for (std::size_t index = 0; index < document.blocks.size(); ++index) {
        if (index > 0) {
            // Items of one list run together; everything else gets a blank
            // line. A bullet item against a numbered one is two lists and not
            // one — Markdown starts a new list wherever the marker changes —
            // so the blank line the author left between them is theirs.
            const Block& block  = document.blocks[index];
            const Block& before = document.blocks[index - 1];
            const bool   same_list = block.kind == BlockKind::ListItem
                && before.kind == BlockKind::ListItem
                && block.ordered == before.ordered;
            out += same_list ? "\n" : "\n\n";
        }
        out += serialize_block(document.blocks[index]);
    }
    if (!out.empty()) {
        out += '\n';
    }
    return out;
}

} // namespace wordsmith::markup
