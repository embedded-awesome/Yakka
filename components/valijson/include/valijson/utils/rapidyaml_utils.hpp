#pragma once

#include <iostream>
#include <string>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include <valijson/utils/file_utils.hpp>

namespace valijson {
namespace utils {

/**
 * @brief  Load a YAML or JSON document from a file into a RapidYAML tree.
 *
 * @param  path      path to the file to load
 * @param  document  tree to populate
 *
 * @returns true if the document was loaded successfully, false otherwise
 */
inline bool loadDocument(const std::string &path, ryml::Tree &document)
{
    std::string content;
    if (!loadFile(path, content)) {
        std::cerr << "Failed to load file '" << path << "'." << std::endl;
        return false;
    }

#if VALIJSON_USE_EXCEPTIONS
    // Install per-tree error callbacks that throw instead of aborting.
    // Both callbacks are non-capturing, so they decay to plain function pointers
    // as required by ryml::Callbacks.
    ryml::Callbacks cb;
    cb.set_error_basic([](ryml::csubstr msg, ryml::ErrorDataBasic const&, void *) {
        throw std::runtime_error(std::string(msg.str, msg.len));
    });
    cb.set_error_parse([](ryml::csubstr msg, ryml::ErrorDataParse const&, void *) {
        throw std::runtime_error(std::string(msg.str, msg.len));
    });
    document = ryml::Tree(cb);

    try {
        ryml::parse_in_arena(ryml::to_csubstr(path),
                             ryml::to_csubstr(content), &document);
    } catch (const std::runtime_error &e) {
        std::cerr << "RapidYAML failed to parse '" << path << "': "
                  << e.what() << std::endl;
        return false;
    }
#else
    ryml::parse_in_arena(ryml::to_csubstr(path),
                         ryml::to_csubstr(content), &document);
#endif

    return true;
}

/**
 * @brief  Navigate to the content node of a RapidYAML document tree.
 *
 * When ryml parses YAML, it creates a stream node at the root with a document
 * node as its first child. For most use cases (single-document YAML/JSON),
 * callers want a ConstNodeRef pointing directly to the document content.
 *
 * @param  tree  the parsed tree
 * @returns ConstNodeRef pointing to the first document's content, or
 *          the root node if the tree layout differs from the expected pattern.
 */
inline ryml::ConstNodeRef getDocumentNode(const ryml::Tree &tree)
{
    ryml::ConstNodeRef root = tree.rootref();
    if (root.is_stream() && root.num_children() > 0) {
        root = root.first_child();
    }
    if (root.is_doc() && root.num_children() > 0) {
        root = root.first_child();
    }
    return root;
}

} // namespace utils
} // namespace valijson
