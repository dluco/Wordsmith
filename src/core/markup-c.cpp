#include "markup-c.h"

#include "markup.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

using wordsmith::markup::Block;
using wordsmith::markup::BlockKind;
using wordsmith::markup::Document;
using wordsmith::markup::Span;

struct WordsmithMarkupDocument {
    Document doc;
};

struct WordsmithMarkupBuilder {
    Document doc;
};

namespace {

WordsmithMarkupBlockKind to_c(BlockKind kind)
{
    switch (kind) {
    case BlockKind::Paragraph: return WORDSMITH_MARKUP_PARAGRAPH;
    case BlockKind::Heading:   return WORDSMITH_MARKUP_HEADING;
    case BlockKind::CodeBlock: return WORDSMITH_MARKUP_CODE_BLOCK;
    case BlockKind::ListItem:  return WORDSMITH_MARKUP_LIST_ITEM;
    case BlockKind::Quote:     return WORDSMITH_MARKUP_QUOTE;
    case BlockKind::Rule:      return WORDSMITH_MARKUP_RULE;
    }
    return WORDSMITH_MARKUP_PARAGRAPH;
}

BlockKind from_c(WordsmithMarkupBlockKind kind)
{
    switch (kind) {
    case WORDSMITH_MARKUP_PARAGRAPH:  return BlockKind::Paragraph;
    case WORDSMITH_MARKUP_HEADING:    return BlockKind::Heading;
    case WORDSMITH_MARKUP_CODE_BLOCK: return BlockKind::CodeBlock;
    case WORDSMITH_MARKUP_LIST_ITEM:  return BlockKind::ListItem;
    case WORDSMITH_MARKUP_QUOTE:      return BlockKind::Quote;
    case WORDSMITH_MARKUP_RULE:       return BlockKind::Rule;
    }
    return BlockKind::Paragraph;
}

const Block* block_at(const WordsmithMarkupDocument* doc, size_t index)
{
    if (doc == nullptr || index >= doc->doc.blocks.size()) {
        return nullptr;
    }
    return &doc->doc.blocks[index];
}

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

} // namespace

/* ── reading ────────────────────────────────────────────────────────────── */

WordsmithMarkupDocument* wordsmith_markup_parse(const char* markdown)
{
    auto* document = new (std::nothrow) WordsmithMarkupDocument();
    if (document == nullptr) {
        return nullptr;
    }
    document->doc = wordsmith::markup::parse(markdown != nullptr ? markdown : "");
    return document;
}

void wordsmith_markup_document_free(WordsmithMarkupDocument* doc)
{
    delete doc;
}

size_t wordsmith_markup_block_count(const WordsmithMarkupDocument* doc)
{
    return doc != nullptr ? doc->doc.blocks.size() : 0;
}

WordsmithMarkupBlockInfo wordsmith_markup_block_info(
    const WordsmithMarkupDocument* doc, size_t block)
{
    WordsmithMarkupBlockInfo info = {WORDSMITH_MARKUP_PARAGRAPH, 0, 0, 0};
    if (const Block* found = block_at(doc, block)) {
        info.kind        = to_c(found->kind);
        info.level       = found->level;
        info.ordered     = found->ordered ? 1 : 0;
        info.list_number = found->list_number;
    }
    return info;
}

const char* wordsmith_markup_block_code(const WordsmithMarkupDocument* doc, size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->code.c_str() : "";
}

const char* wordsmith_markup_block_language(const WordsmithMarkupDocument* doc,
                                             size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->language.c_str() : "";
}

size_t wordsmith_markup_block_span_count(const WordsmithMarkupDocument* doc,
                                          size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->spans.size() : 0;
}

WordsmithMarkupSpan wordsmith_markup_block_span(const WordsmithMarkupDocument* doc,
                                                  size_t block, size_t span)
{
    WordsmithMarkupSpan out = {"", 0, nullptr};
    const Block* found = block_at(doc, block);
    if (found == nullptr || span >= found->spans.size()) {
        return out;
    }

    const Span& source = found->spans[span];
    out.text  = source.text.c_str();
    out.flags = (source.emphasis  ? WORDSMITH_MARKUP_SPAN_EMPHASIS  : 0u)
              | (source.strong    ? WORDSMITH_MARKUP_SPAN_STRONG    : 0u)
              | (source.underline ? WORDSMITH_MARKUP_SPAN_UNDERLINE : 0u)
              | (source.code      ? WORDSMITH_MARKUP_SPAN_CODE      : 0u);
    out.href  = source.href.empty() ? nullptr : source.href.c_str();
    return out;
}

/* ── writing ────────────────────────────────────────────────────────────── */

WordsmithMarkupBuilder* wordsmith_markup_builder_new(void)
{
    return new (std::nothrow) WordsmithMarkupBuilder();
}

void wordsmith_markup_builder_free(WordsmithMarkupBuilder* builder)
{
    delete builder;
}

void wordsmith_markup_builder_begin_block(WordsmithMarkupBuilder* builder,
                                           WordsmithMarkupBlockKind kind,
                                           int level)
{
    if (builder == nullptr) {
        return;
    }
    Block block;
    block.kind  = from_c(kind);
    block.level = level;
    builder->doc.blocks.push_back(std::move(block));
}

void wordsmith_markup_builder_add_span(WordsmithMarkupBuilder* builder,
                                        const char* text, uint32_t flags,
                                        const char* href)
{
    if (builder == nullptr || builder->doc.blocks.empty() || text == nullptr) {
        return;
    }
    Span span;
    span.text      = text;
    span.emphasis  = (flags & WORDSMITH_MARKUP_SPAN_EMPHASIS) != 0;
    span.strong    = (flags & WORDSMITH_MARKUP_SPAN_STRONG) != 0;
    span.underline = (flags & WORDSMITH_MARKUP_SPAN_UNDERLINE) != 0;
    span.code      = (flags & WORDSMITH_MARKUP_SPAN_CODE) != 0;
    if (href != nullptr) {
        span.href = href;
    }
    builder->doc.blocks.back().spans.push_back(std::move(span));
}

void wordsmith_markup_builder_set_code(WordsmithMarkupBuilder* builder,
                                        const char* code, const char* language)
{
    if (builder == nullptr || builder->doc.blocks.empty()) {
        return;
    }
    Block& block = builder->doc.blocks.back();
    block.code = code != nullptr ? code : "";
    block.language = language != nullptr ? language : "";
}

char* wordsmith_markup_builder_to_markdown(const WordsmithMarkupBuilder* builder)
{
    if (builder == nullptr) {
        return duplicate(std::string());
    }
    return duplicate(wordsmith::markup::serialize(builder->doc));
}

void wordsmith_free_string(char* text)
{
    std::free(text);
}
