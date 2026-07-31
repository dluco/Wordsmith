#include "yaml.hpp"

#include <cctype>
#include <cstdlib>

namespace wordsmith::yaml {

const Node* Node::find(std::string_view key) const
{
    if (kind != Kind::Map) {
        return nullptr;
    }
    auto it = map.find(std::string(key));
    return it == map.end() ? nullptr : &it->second;
}

std::optional<std::string> Node::string(std::string_view key) const
{
    const Node* node = find(key);
    if (node == nullptr || !node->is_scalar()) {
        return std::nullopt;
    }
    return node->scalar;
}

std::optional<long> Node::integer(std::string_view key) const
{
    auto text = string(key);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    std::size_t i   = 0;
    bool        neg = false;
    if ((*text)[i] == '-' || (*text)[i] == '+') {
        neg = (*text)[i] == '-';
        ++i;
    }
    if (i >= text->size()) {
        return std::nullopt;
    }
    long value = 0;
    for (; i < text->size(); ++i) {
        const char ch = (*text)[i];
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        value = value * 10 + (ch - '0');
    }
    return neg ? -value : value;
}

std::optional<bool> Node::boolean(std::string_view key) const
{
    auto text = string(key);
    if (!text) {
        return std::nullopt;
    }
    std::string lower;
    lower.reserve(text->size());
    for (char ch : *text) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "true" || lower == "yes" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "off") {
        return false;
    }
    return std::nullopt;
}

void Node::set(std::string key, Node child)
{
    if (map.find(key) == map.end()) {
        order.push_back(key);
    }
    map[std::move(key)] = std::move(child);
}

namespace {

constexpr std::size_t NPOS = std::string_view::npos;

bool is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

std::string_view rstrip(std::string_view text)
{
    std::size_t end = text.size();
    while (end > 0 && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(0, end);
}

/* One physical line, with the byte offsets the ranges in Node are built from.
 * Offsets are into the text handed to tokenize(); `base` is added later. */
struct Line {
    std::size_t begin         = 0;  // first byte of the line
    std::size_t end           = 0;  // one past its newline, or end of text
    std::size_t content_begin = 0;  // first byte after the indent
    std::size_t content_end   = 0;  // one past the last non-space byte
    std::size_t number        = 0;  // 1-based, for diagnostics
    int         indent        = 0;
    bool        blank         = false;
    bool        comment       = false;  // whole line is a comment
    bool        tab_indent    = false;
    std::string content;                // [content_begin, content_end)
};

std::vector<Line> tokenize(std::string_view text)
{
    std::vector<Line> lines;
    std::size_t       pos    = 0;
    std::size_t       number = 0;

    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        const std::size_t stop    = newline == NPOS ? text.size() : newline;
        const std::size_t next    = newline == NPOS ? text.size() : newline + 1;

        std::size_t content_end = stop;
        while (content_end > pos && is_space(text[content_end - 1])) {
            --content_end;
        }
        std::size_t content_begin = pos;
        while (content_begin < content_end && text[content_begin] == ' ') {
            ++content_begin;
        }

        Line line;
        line.begin         = pos;
        line.end           = next;
        line.content_begin = content_begin;
        line.content_end   = content_end;
        line.number        = ++number;
        line.indent        = static_cast<int>(content_begin - pos);
        line.content = std::string(text.substr(content_begin, content_end - content_begin));
        line.blank   = line.content.empty();
        line.comment = !line.blank && line.content[0] == '#';
        /* YAML forbids tabs in indentation, and a file using them will parse in
         * ways its author did not intend. Worth saying so rather than guessing. */
        line.tab_indent = content_begin < content_end && text[content_begin] == '\t';

        lines.push_back(std::move(line));
        pos = next;
    }
    return lines;
}

/* Index of the `#` starting a trailing comment, or NPOS. A `#` only opens a
 * comment at the start of the value or after whitespace, and never inside a
 * quoted scalar. */
std::size_t find_comment(std::string_view text)
{
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (quote != 0) {
            if (ch == '\\' && quote == '"' && i + 1 < text.size()) {
                ++i;
            } else if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
            return i;
        }
    }
    return NPOS;
}

std::string decode_double_quoted(std::string_view body)
{
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 >= body.size()) {
            out += body[i];
            continue;
        }
        switch (body[++i]) {
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case '0':  out += '\0'; break;
        case '"':  out += '"';  break;
        case '\\': out += '\\'; break;
        case '/':  out += '/';  break;
        default:   out += '\\'; out += body[i]; break;
        }
    }
    return out;
}

std::string decode_single_quoted(std::string_view body)
{
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        out += body[i];
        if (body[i] == '\'' && i + 1 < body.size() && body[i + 1] == '\'') {
            ++i;
        }
    }
    return out;
}

/* Turn the raw bytes of a scalar into its value: unquote, unescape. Plain
 * scalars pass through, already trimmed and comment-stripped by the caller. */
std::string decode_scalar(std::string_view raw)
{
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return decode_double_quoted(raw.substr(1, raw.size() - 2));
    }
    if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
        return decode_single_quoted(raw.substr(1, raw.size() - 2));
    }
    return std::string(raw);
}

/* Split the body of a flow sequence on commas that are not inside quotes.
 * Returns false if a quote is left open. */
bool split_flow(std::string_view body, std::vector<std::string>& items)
{
    std::string current;
    char        quote = 0;

    auto flush = [&]() {
        std::string_view trimmed = rstrip(current);
        std::size_t      start   = 0;
        while (start < trimmed.size() && trimmed[start] == ' ') {
            ++start;
        }
        trimmed = trimmed.substr(start);
        if (!trimmed.empty()) {
            items.push_back(decode_scalar(trimmed));
        }
        current.clear();
    };

    for (std::size_t i = 0; i < body.size(); ++i) {
        const char ch = body[i];
        if (quote != 0) {
            current += ch;
            if (ch == '\\' && quote == '"' && i + 1 < body.size()) {
                current += body[++i];
            } else if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            current += ch;
        } else if (ch == ',') {
            flush();
        } else {
            current += ch;
        }
    }
    if (quote != 0) {
        return false;
    }
    flush();
    return true;
}

/* A `|` or `>` header, with its chomping indicator. */
struct BlockHeader {
    bool folded = false;  // `>` rather than `|`
    char chomp  = 0;      // '-' strip, '+' keep, 0 clip
};

bool parse_block_header(std::string_view raw, BlockHeader& header)
{
    if (raw.empty() || (raw[0] != '|' && raw[0] != '>')) {
        return false;
    }
    header.folded = raw[0] == '>';
    header.chomp  = 0;
    /* An explicit indentation indicator (`|2`) is the one header piece we do
     * not honour; the block still parses, just with the indent inferred. */
    for (std::size_t i = 1; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (ch == '-' || ch == '+') {
            header.chomp = ch;
        } else if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    Parser(std::string_view text, const std::vector<Line>& lines, std::size_t base,
           std::vector<Diagnostic>* diagnostics)
        : text_(text), lines_(lines), base_(base), diagnostics_(diagnostics)
    {
    }

    Node parse_document()
    {
        Node root = parse_map(0);
        root.entry = Range{};
        root.value = Range{};
        return root;
    }

private:
    Range range(std::size_t begin, std::size_t end) const
    {
        return Range{begin + base_, end + base_};
    }

    void report(const Line& line, std::string message)
    {
        if (diagnostics_ != nullptr) {
            diagnostics_->push_back(Diagnostic{line.number, std::move(message)});
        }
    }

    bool skippable(const Line& line) const { return line.blank || line.comment; }

    /* `- item` or a bare `-`, as opposed to a key that merely starts with a
     * hyphen. */
    static bool is_sequence_marker(const Line& line)
    {
        return !line.content.empty() && line.content[0] == '-'
            && (line.content.size() == 1 || line.content[1] == ' ');
    }

    /* Index of the next line carrying content, or lines_.size(). */
    std::size_t next_content(std::size_t from) const
    {
        while (from < lines_.size() && skippable(lines_[from])) {
            ++from;
        }
        return from;
    }

    /* Split a mapping line. `value_begin`/`value_end` bound the raw scalar,
     * comment and surrounding space removed; they are equal when the line is
     * just `key:`. */
    bool split_entry(const Line& line, std::string& key, std::size_t& value_begin,
                     std::size_t& value_end) const
    {
        const std::string& content = line.content;

        std::size_t colon = NPOS;
        char        quote = 0;
        for (std::size_t i = 0; i < content.size(); ++i) {
            const char ch = content[i];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
                continue;
            }
            if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == ':'
                       && (i + 1 == content.size() || content[i + 1] == ' '
                           || content[i + 1] == '\t')) {
                colon = i;
                break;
            }
        }
        if (colon == NPOS) {
            return false;
        }

        key = decode_scalar(rstrip(std::string_view(content).substr(0, colon)));
        if (key.empty()) {
            return false;
        }

        std::size_t start = colon + 1;
        while (start < content.size() && (content[start] == ' ' || content[start] == '\t')) {
            ++start;
        }
        std::string_view rest    = std::string_view(content).substr(start);
        const std::size_t comment = find_comment(rest);
        if (comment != NPOS) {
            rest = rest.substr(0, comment);
        }
        rest = rstrip(rest);

        value_begin = line.content_begin + start;
        value_end   = value_begin + rest.size();
        return true;
    }

    /* Consume a block scalar whose header sits on lines_[i_ - 1]. `value_end`
     * is advanced to the end of the last content line. */
    std::string parse_block_scalar(const BlockHeader& header, int key_indent,
                                   std::size_t& value_end)
    {
        std::vector<std::string> content;   // content lines, block indent removed
        int                      indent = -1;
        std::size_t              last   = NPOS;
        const std::size_t        start  = i_;

        while (i_ < lines_.size()) {
            const Line& line = lines_[i_];
            if (!line.blank && line.indent <= key_indent) {
                break;
            }
            if (line.blank) {
                content.emplace_back();
                ++i_;
                continue;
            }
            /* The block's indent is set by its first line; deeper lines keep
             * the extra indentation as part of their text. */
            if (indent < 0) {
                indent = line.indent;
            }
            const std::size_t from = line.begin + static_cast<std::size_t>(indent);
            content.push_back(std::string(text_.substr(from, line.content_end - from)));
            last = i_;
            ++i_;
        }

        if (last == NPOS) {
            i_ = start;
            return std::string();
        }
        value_end = lines_[last].content_end;

        /* Blank lines past the last content line belong to the file, not the
         * scalar; rewind so the entry's range stops at real content. */
        i_        = last + 1;
        consumed_ = last;
        std::size_t trailing = content.size();
        while (trailing > 0 && content[trailing - 1].empty()) {
            --trailing;
        }
        const std::size_t blanks = content.size() - trailing;
        content.resize(trailing);

        std::string out;
        if (!header.folded) {
            for (std::size_t n = 0; n < content.size(); ++n) {
                if (n > 0) {
                    out += '\n';
                }
                out += content[n];
            }
        } else {
            /* Folding turns a single line break into a space, and a run of n
             * breaks into n - 1 of them — so one blank line separates
             * paragraphs with exactly one newline. More-indented lines keep
             * their breaks in real YAML; that refinement is not modelled. */
            for (std::size_t n = 0; n < content.size();) {
                std::size_t blanks = 0;
                while (n + blanks < content.size() && content[n + blanks].empty()) {
                    ++blanks;
                }
                if (blanks > 0) {
                    out.append(blanks, '\n');
                    n += blanks;
                    continue;
                }
                if (!out.empty() && out.back() != '\n') {
                    out += ' ';
                }
                out += content[n];
                ++n;
            }
        }

        if (header.chomp == '-') {
            return out;
        }
        if (!out.empty() || header.chomp == '+') {
            out += '\n';
        }
        if (header.chomp == '+') {
            out.append(blanks, '\n');
        }
        return out;
    }

    Node parse_map(int base_indent)
    {
        Node node;
        node.kind = Node::Kind::Map;

        while (i_ < lines_.size()) {
            const Line& line = lines_[i_];
            if (skippable(line)) {
                ++i_;
                continue;
            }
            if (line.indent < base_indent) {
                break;
            }
            if (is_sequence_marker(line)) {
                break;  // a sequence: it belongs to the caller
            }
            if (line.indent > base_indent) {
                report(line, "unexpected indentation; line skipped");
                ++i_;
                continue;
            }
            if (line.tab_indent) {
                report(line, "tab used for indentation; YAML requires spaces");
            }

            std::string key;
            std::size_t value_begin = 0;
            std::size_t value_end   = 0;
            if (!split_entry(line, key, value_begin, value_end)) {
                report(line, "not a `key: value` line; skipped");
                ++i_;
                continue;
            }

            const std::size_t entry_begin = line.begin;
            consumed_                     = i_;
            ++i_;

            Node child  = parse_value(base_indent, line, value_begin, value_end);
            child.entry = range(entry_begin, lines_[consumed_].end);
            child.value = range(value_begin, value_end);
            node.set(std::move(key), std::move(child));
        }
        return node;
    }

    /* The value of the entry whose key line is `line`. Consumes any block
     * scalar, block sequence, or nested mapping that follows it, widening
     * `value_begin`/`value_end`/`entry_end` over what it took. */
    Node parse_value(int base_indent, const Line& line, std::size_t& value_begin,
                     std::size_t& value_end)
    {
        std::string_view raw = text_.substr(value_begin, value_end - value_begin);

        if (!raw.empty()) {
            BlockHeader header;
            if (parse_block_header(raw, header)) {
                Node child;
                child.kind   = Node::Kind::Scalar;
                child.scalar = parse_block_scalar(header, line.indent, value_end);
                return child;
            }
            if (raw.front() == '[') {
                Node child;
                child.kind = Node::Kind::Sequence;
                if (raw.back() != ']') {
                    report(line, "unterminated flow sequence");
                    child.kind   = Node::Kind::Scalar;
                    child.scalar = decode_scalar(raw);
                    return child;
                }
                std::vector<std::string> items;
                if (!split_flow(raw.substr(1, raw.size() - 2), items)) {
                    report(line, "unterminated quote in flow sequence");
                }
                for (std::string& item : items) {
                    Node element;
                    element.kind   = Node::Kind::Scalar;
                    element.scalar = std::move(item);
                    child.seq.push_back(std::move(element));
                }
                return child;
            }
            if (raw.front() == '{') {
                report(line, "flow mappings are not supported; kept as text");
            }

            Node child;
            child.kind   = Node::Kind::Scalar;
            child.scalar = decode_scalar(raw);
            return child;
        }

        /* Empty value: a block sequence or nested mapping may follow. A
         * sequence may sit at the key's own indent, which is how frontmatter
         * usually writes tags. */
        const std::size_t next = next_content(i_);
        if (next < lines_.size()) {
            const Line& following = lines_[next];
            /* `value_begin` stays where the key line left it, just past the
             * colon, so the range covers the newline and indent leading into
             * the block. Replacing the field then has somewhere to put its
             * value rather than landing on the line below. */
            if (is_sequence_marker(following) && following.indent >= base_indent) {
                i_ = next;
                return parse_sequence(following.indent, value_end);
            }
            if (!is_sequence_marker(following) && following.indent > base_indent) {
                i_         = next;
                Node child = parse_map(following.indent);
                value_end  = lines_[consumed_].content_end;
                return child;
            }
        }
        return Node{};  // empty scalar
    }

    Node parse_sequence(int base_indent, std::size_t& value_end)
    {
        Node node;
        node.kind = Node::Kind::Sequence;

        std::size_t last = NPOS;

        while (i_ < lines_.size()) {
            const Line& line = lines_[i_];
            if (skippable(line)) {
                ++i_;
                continue;
            }
            if (line.indent != base_indent || !is_sequence_marker(line)) {
                break;
            }

            std::size_t item_begin = line.content_begin + 1;  // past the '-'
            while (item_begin < line.content_end && text_[item_begin] == ' ') {
                ++item_begin;
            }
            std::string_view  item    = text_.substr(item_begin, line.content_end - item_begin);
            const std::size_t comment = find_comment(item);
            if (comment != NPOS) {
                item = item.substr(0, comment);
            }
            item = rstrip(item);

            Node element;
            element.kind   = Node::Kind::Scalar;
            element.scalar = decode_scalar(item);
            element.entry  = range(line.begin, line.end);
            element.value  = range(item_begin, item_begin + item.size());
            node.seq.push_back(std::move(element));

            last = i_;
            ++i_;

            /* Lines indented under an item would make it a map or a nested
             * sequence. We do not model those, so skip them rather than
             * mistake them for siblings. */
            while (i_ < lines_.size()
                   && (skippable(lines_[i_]) || lines_[i_].indent > base_indent)) {
                if (!skippable(lines_[i_])) {
                    report(lines_[i_], "nested sequence items are not supported; skipped");
                    last = i_;
                }
                ++i_;
            }
        }

        if (last == NPOS) {
            return node;  // no items after all; ranges stay as the caller set them
        }
        i_        = last + 1;
        consumed_ = last;
        value_end = lines_[last].content_end;
        return node;
    }

    std::string_view         text_;
    const std::vector<Line>& lines_;
    std::size_t              base_        = 0;
    std::vector<Diagnostic>* diagnostics_ = nullptr;
    std::size_t              i_           = 0;
    /* Index of the last line that was part of an entry, as opposed to blank or
     * comment lines swallowed on the way past. Entry ranges end here. */
    std::size_t consumed_ = 0;
};

bool looks_numeric(std::string_view text)
{
    if (text.empty()) {
        return false;
    }
    char*             end   = nullptr;
    const std::string owned(text);
    std::strtod(owned.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool is_reserved_word(std::string_view text)
{
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower == "true" || lower == "false" || lower == "yes" || lower == "no"
        || lower == "on" || lower == "off" || lower == "null" || lower == "~";
}

bool needs_quoting(std::string_view text)
{
    if (text.empty()) {
        return true;
    }
    if (is_space(text.front()) || is_space(text.back())) {
        return true;
    }
    if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(text.front()) != NPOS) {
        return true;
    }
    if (text.find(": ") != NPOS || text.find(" #") != NPOS) {
        return true;
    }
    for (char ch : text) {
        if (static_cast<unsigned char>(ch) < 0x20) {
            return true;
        }
    }
    return is_reserved_word(text) || looks_numeric(text);
}

} // namespace

Node parse(std::string_view text, std::size_t base, std::vector<Diagnostic>* diagnostics)
{
    const std::vector<Line> lines = tokenize(text);
    Parser                  parser(text, lines, base, diagnostics);
    return parser.parse_document();
}

std::string encode_scalar(std::string_view value, int indent)
{
    if (value.find('\n') != NPOS) {
        /* A single trailing newline is what YAML's default chomping puts back,
         * so `|` reproduces it and `|-` reproduces its absence. */
        std::string_view body = value;
        const bool        clip = body.back() == '\n';
        if (clip) {
            body = body.substr(0, body.size() - 1);
        }

        const std::string pad(static_cast<std::size_t>(indent) + 2, ' ');
        std::string       out = clip ? "|\n" : "|-\n";
        std::size_t       pos = 0;
        while (pos <= body.size()) {
            const std::size_t newline = body.find('\n', pos);
            const std::size_t stop    = newline == NPOS ? body.size() : newline;
            const std::string_view line = body.substr(pos, stop - pos);
            if (!line.empty()) {
                out += pad;
                out += line;
            }
            if (newline == NPOS) {
                break;
            }
            out += '\n';
            pos = newline + 1;
        }
        return out;
    }

    if (!needs_quoting(value)) {
        return std::string(value);
    }

    std::string out = "\"";
    for (char ch : value) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        default:   out += ch;     break;
        }
    }
    out += '"';
    return out;
}

} // namespace wordsmith::yaml
