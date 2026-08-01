#include "frontmatter-c.h"

#include "frontmatter.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

using wordsmith::frontmatter::Document;
using wordsmith::yaml::Node;

struct WordsmithFrontmatter {
    Document doc;
    /* Diagnostics are rendered once, at parse time, so the accessors can hand
     * out borrowed pointers like the rest of this header does. */
    std::vector<std::string> messages;
};

namespace {

/* malloc'd copy, so callers can free with wordsmith_free_string() regardless
 * of which allocator the C++ side used. */
char* duplicate(const std::string& text)
{
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

const Node* lookup(const WordsmithFrontmatter* frontmatter, const char* key)
{
    if (frontmatter == nullptr || key == nullptr) {
        return nullptr;
    }
    return frontmatter->doc.root.find(key);
}

} // namespace

WordsmithFrontmatter* wordsmith_frontmatter_parse(const char* text)
{
    auto* frontmatter = new (std::nothrow) WordsmithFrontmatter();
    if (frontmatter == nullptr) {
        return nullptr;
    }
    frontmatter->doc = wordsmith::frontmatter::parse(text != nullptr ? text : "");
    for (const auto& diagnostic : frontmatter->doc.diagnostics) {
        frontmatter->messages.push_back(diagnostic.message);
    }
    return frontmatter;
}

WordsmithFrontmatter* wordsmith_frontmatter_parse_yaml(const char* text)
{
    auto* frontmatter = new (std::nothrow) WordsmithFrontmatter();
    if (frontmatter == nullptr) {
        return nullptr;
    }
    frontmatter->doc.root = wordsmith::yaml::parse(
        text != nullptr ? text : "", 0, &frontmatter->doc.diagnostics);
    for (const auto& diagnostic : frontmatter->doc.diagnostics) {
        frontmatter->messages.push_back(diagnostic.message);
    }
    return frontmatter;
}

void wordsmith_frontmatter_free(WordsmithFrontmatter* frontmatter) { delete frontmatter; }

int wordsmith_frontmatter_present(const WordsmithFrontmatter* frontmatter)
{
    return frontmatter != nullptr && frontmatter->doc.split.present ? 1 : 0;
}

size_t wordsmith_frontmatter_body_offset(const WordsmithFrontmatter* frontmatter)
{
    return frontmatter != nullptr ? frontmatter->doc.split.body_begin : 0;
}

size_t wordsmith_frontmatter_key_count(const WordsmithFrontmatter* frontmatter)
{
    return frontmatter != nullptr ? frontmatter->doc.root.order.size() : 0;
}

const char* wordsmith_frontmatter_key_at(const WordsmithFrontmatter* frontmatter,
                                         size_t index)
{
    if (frontmatter == nullptr || index >= frontmatter->doc.root.order.size()) {
        return nullptr;
    }
    return frontmatter->doc.root.order[index].c_str();
}

WordsmithFrontmatterValueKind
wordsmith_frontmatter_value_kind(const WordsmithFrontmatter* frontmatter, const char* key)
{
    const Node* node = lookup(frontmatter, key);
    if (node == nullptr) {
        return WORDSMITH_FRONTMATTER_MISSING;
    }
    switch (node->kind) {
    case Node::Kind::Scalar:   return WORDSMITH_FRONTMATTER_SCALAR;
    case Node::Kind::Sequence: return WORDSMITH_FRONTMATTER_SEQUENCE;
    case Node::Kind::Map:      return WORDSMITH_FRONTMATTER_MAP;
    }
    return WORDSMITH_FRONTMATTER_MISSING;
}

const char* wordsmith_frontmatter_string(const WordsmithFrontmatter* frontmatter,
                                         const char* key)
{
    const Node* node = lookup(frontmatter, key);
    if (node == nullptr || !node->is_scalar()) {
        return nullptr;
    }
    return node->scalar.c_str();
}

size_t wordsmith_frontmatter_sequence_count(const WordsmithFrontmatter* frontmatter,
                                            const char* key)
{
    const Node* node = lookup(frontmatter, key);
    return node != nullptr && node->is_sequence() ? node->seq.size() : 0;
}

const char* wordsmith_frontmatter_sequence_at(const WordsmithFrontmatter* frontmatter,
                                              const char* key, size_t index)
{
    const Node* node = lookup(frontmatter, key);
    if (node == nullptr || !node->is_sequence() || index >= node->seq.size()) {
        return nullptr;
    }
    return node->seq[index].scalar.c_str();
}

size_t wordsmith_frontmatter_diagnostic_count(const WordsmithFrontmatter* frontmatter)
{
    return frontmatter != nullptr ? frontmatter->messages.size() : 0;
}

const char* wordsmith_frontmatter_diagnostic_at(const WordsmithFrontmatter* frontmatter,
                                                size_t index, size_t* line)
{
    if (frontmatter == nullptr || index >= frontmatter->messages.size()) {
        return nullptr;
    }
    if (line != nullptr) {
        *line = frontmatter->doc.diagnostics[index].line;
    }
    return frontmatter->messages[index].c_str();
}

char* wordsmith_frontmatter_set_field(const char* text, const char* key,
                                      const char* value)
{
    if (text == nullptr || key == nullptr) {
        return nullptr;
    }
    std::optional<std::string_view> replacement;
    if (value != nullptr) {
        replacement = std::string_view(value);
    }
    return duplicate(wordsmith::frontmatter::set_field(text, key, replacement));
}
