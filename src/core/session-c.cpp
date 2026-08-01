#include "session-c.h"

#include "session.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* Absolute paths, so callers never have to know the file holds relative ones. */
struct WordsmithSession {
    std::string              open_document;
    std::vector<std::string> expanded;
};

namespace {

char* duplicate(const std::string& text)
{
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

void set_error(char** error, const std::string& message)
{
    if (error != nullptr) {
        *error = duplicate(message);
    }
}

} // namespace

WordsmithSession* wordsmith_session_load(const char* root)
{
    WordsmithSession* session = new WordsmithSession();
    if (root == nullptr) {
        return session;
    }

    const fs::path project_root(root);
    const wordsmith::ProjectSession saved =
        wordsmith::session_for(wordsmith::session_path(), project_root);

    if (!saved.open_document.empty()) {
        session->open_document =
            wordsmith::session_absolute(project_root, saved.open_document).string();
    }
    for (const std::string& folder : saved.expanded) {
        session->expanded.push_back(
            wordsmith::session_absolute(project_root, folder).string());
    }
    return session;
}

void wordsmith_session_free(WordsmithSession* session)
{
    delete session;
}

const char* wordsmith_session_open_document(const WordsmithSession* session)
{
    if (session == nullptr || session->open_document.empty()) {
        return nullptr;
    }
    return session->open_document.c_str();
}

size_t wordsmith_session_expanded_count(const WordsmithSession* session)
{
    return session != nullptr ? session->expanded.size() : 0;
}

const char* wordsmith_session_expanded(const WordsmithSession* session, size_t index)
{
    if (session == nullptr || index >= session->expanded.size()) {
        return nullptr;
    }
    return session->expanded[index].c_str();
}

int wordsmith_session_save(const char* root, const char* open_document,
                           const char* const* expanded, size_t count, char** error)
{
    if (root == nullptr) {
        set_error(error, "no project to save the session of");
        return 0;
    }

    const fs::path project_root(root);
    wordsmith::ProjectSession session;
    session.root = project_root.string();

    if (open_document != nullptr) {
        session.open_document = wordsmith::session_relative(project_root, open_document);
    }
    for (size_t index = 0; index < count && expanded != nullptr; index++) {
        if (expanded[index] == nullptr) {
            continue;
        }
        const std::string relative =
            wordsmith::session_relative(project_root, expanded[index]);
        if (!relative.empty()) {
            session.expanded.push_back(relative);
        }
    }

    std::string message;
    if (!wordsmith::save_project_session(session, wordsmith::session_path(), message)) {
        set_error(error, message);
        return 0;
    }
    return 1;
}
