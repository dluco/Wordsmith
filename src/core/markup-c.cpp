#include "markup-c.h"

#include "markup.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

using wordsworth::markup::Block;
using wordsworth::markup::BlockKind;
using wordsworth::markup::Document;
using wordsworth::markup::Span;

struct WordsworthMarkupDocument {
    Document doc;
};

struct WordsworthMarkupBuilder {
    Document doc;
};

namespace {

WordsworthMarkupBlockKind to_c(BlockKind kind)
{
    switch (kind) {
    case BlockKind::Paragraph: return WORDSWORTH_MARKUP_PARAGRAPH;
    case BlockKind::Heading:   return WORDSWORTH_MARKUP_HEADING;
    case BlockKind::CodeBlock: return WORDSWORTH_MARKUP_CODE_BLOCK;
    case BlockKind::ListItem:  return WORDSWORTH_MARKUP_LIST_ITEM;
    case BlockKind::Quote:     return WORDSWORTH_MARKUP_QUOTE;
    case BlockKind::Rule:      return WORDSWORTH_MARKUP_RULE;
    }
    return WORDSWORTH_MARKUP_PARAGRAPH;
}

BlockKind from_c(WordsworthMarkupBlockKind kind)
{
    switch (kind) {
    case WORDSWORTH_MARKUP_PARAGRAPH:  return BlockKind::Paragraph;
    case WORDSWORTH_MARKUP_HEADING:    return BlockKind::Heading;
    case WORDSWORTH_MARKUP_CODE_BLOCK: return BlockKind::CodeBlock;
    case WORDSWORTH_MARKUP_LIST_ITEM:  return BlockKind::ListItem;
    case WORDSWORTH_MARKUP_QUOTE:      return BlockKind::Quote;
    case WORDSWORTH_MARKUP_RULE:       return BlockKind::Rule;
    }
    return BlockKind::Paragraph;
}

const Block* block_at(const WordsworthMarkupDocument* doc, size_t index)
{
    if (doc == nullptr || index >= doc->doc.blocks.size()) {
        return nullptr;
    }
    return &doc->doc.blocks[index];
}

/* malloc'd copy, so callers can free with wordsworth_free_string() regardless
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

WordsworthMarkupDocument* wordsworth_markup_parse(const char* markdown)
{
    auto* document = new (std::nothrow) WordsworthMarkupDocument();
    if (document == nullptr) {
        return nullptr;
    }
    document->doc = wordsworth::markup::parse(markdown != nullptr ? markdown : "");
    return document;
}

void wordsworth_markup_document_free(WordsworthMarkupDocument* doc)
{
    delete doc;
}

size_t wordsworth_markup_block_count(const WordsworthMarkupDocument* doc)
{
    return doc != nullptr ? doc->doc.blocks.size() : 0;
}

WordsworthMarkupBlockInfo wordsworth_markup_block_info(
    const WordsworthMarkupDocument* doc, size_t block)
{
    WordsworthMarkupBlockInfo info = {WORDSWORTH_MARKUP_PARAGRAPH, 0, 0, 0};
    if (const Block* found = block_at(doc, block)) {
        info.kind        = to_c(found->kind);
        info.level       = found->level;
        info.ordered     = found->ordered ? 1 : 0;
        info.list_number = found->list_number;
    }
    return info;
}

const char* wordsworth_markup_block_code(const WordsworthMarkupDocument* doc, size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->code.c_str() : "";
}

const char* wordsworth_markup_block_language(const WordsworthMarkupDocument* doc,
                                             size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->language.c_str() : "";
}

size_t wordsworth_markup_block_span_count(const WordsworthMarkupDocument* doc,
                                          size_t block)
{
    const Block* found = block_at(doc, block);
    return found != nullptr ? found->spans.size() : 0;
}

WordsworthMarkupSpan wordsworth_markup_block_span(const WordsworthMarkupDocument* doc,
                                                  size_t block, size_t span)
{
    WordsworthMarkupSpan out = {"", 0, nullptr};
    const Block* found = block_at(doc, block);
    if (found == nullptr || span >= found->spans.size()) {
        return out;
    }

    const Span& source = found->spans[span];
    out.text  = source.text.c_str();
    out.flags = (source.emphasis  ? WORDSWORTH_MARKUP_SPAN_EMPHASIS  : 0u)
              | (source.strong    ? WORDSWORTH_MARKUP_SPAN_STRONG    : 0u)
              | (source.underline ? WORDSWORTH_MARKUP_SPAN_UNDERLINE : 0u)
              | (source.code      ? WORDSWORTH_MARKUP_SPAN_CODE      : 0u);
    out.href  = source.href.empty() ? nullptr : source.href.c_str();
    return out;
}

/* ── writing ────────────────────────────────────────────────────────────── */

WordsworthMarkupBuilder* wordsworth_markup_builder_new(void)
{
    return new (std::nothrow) WordsworthMarkupBuilder();
}

void wordsworth_markup_builder_free(WordsworthMarkupBuilder* builder)
{
    delete builder;
}

void wordsworth_markup_builder_begin_block(WordsworthMarkupBuilder* builder,
                                           WordsworthMarkupBlockKind kind,
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

void wordsworth_markup_builder_add_span(WordsworthMarkupBuilder* builder,
                                        const char* text, uint32_t flags,
                                        const char* href)
{
    if (builder == nullptr || builder->doc.blocks.empty() || text == nullptr) {
        return;
    }
    Span span;
    span.text      = text;
    span.emphasis  = (flags & WORDSWORTH_MARKUP_SPAN_EMPHASIS) != 0;
    span.strong    = (flags & WORDSWORTH_MARKUP_SPAN_STRONG) != 0;
    span.underline = (flags & WORDSWORTH_MARKUP_SPAN_UNDERLINE) != 0;
    span.code      = (flags & WORDSWORTH_MARKUP_SPAN_CODE) != 0;
    if (href != nullptr) {
        span.href = href;
    }
    builder->doc.blocks.back().spans.push_back(std::move(span));
}

void wordsworth_markup_builder_set_code(WordsworthMarkupBuilder* builder,
                                        const char* code, const char* language)
{
    if (builder == nullptr || builder->doc.blocks.empty()) {
        return;
    }
    Block& block = builder->doc.blocks.back();
    block.code = code != nullptr ? code : "";
    block.language = language != nullptr ? language : "";
}

char* wordsworth_markup_builder_to_markdown(const WordsworthMarkupBuilder* builder)
{
    if (builder == nullptr) {
        return duplicate(std::string());
    }
    return duplicate(wordsworth::markup::serialize(builder->doc));
}

void wordsworth_free_string(char* text)
{
    std::free(text);
}
