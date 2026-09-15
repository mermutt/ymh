#include "ymh/execution/git_service.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <git2.h>

#include "ymh/execution/errors.hpp"

namespace ymh {
namespace {

struct RepoDeleter {
    void operator()(git_repository* repo) const noexcept { git_repository_free(repo); }
};
struct RefDeleter {
    void operator()(git_reference* ref) const noexcept { git_reference_free(ref); }
};
struct StatusListDeleter {
    void operator()(git_status_list* list) const noexcept { git_status_list_free(list); }
};
struct IndexDeleter {
    void operator()(git_index* index) const noexcept { git_index_free(index); }
};
struct DiffDeleter {
    void operator()(git_diff* diff) const noexcept { git_diff_free(diff); }
};
struct TreeDeleter {
    void operator()(git_tree* tree) const noexcept { git_tree_free(tree); }
};
struct ObjectDeleter {
    void operator()(git_object* object) const noexcept { git_object_free(object); }
};

using RepoPtr       = std::unique_ptr<git_repository, RepoDeleter>;
using RefPtr        = std::unique_ptr<git_reference, RefDeleter>;
using StatusListPtr = std::unique_ptr<git_status_list, StatusListDeleter>;
using IndexPtr      = std::unique_ptr<git_index, IndexDeleter>;
using DiffPtr       = std::unique_ptr<git_diff, DiffDeleter>;
using TreePtr       = std::unique_ptr<git_tree, TreeDeleter>;
using ObjectPtr     = std::unique_ptr<git_object, ObjectDeleter>;

class BufGuard {
public:
    BufGuard() { buf_ = GIT_BUF_INIT; }
    ~BufGuard() { git_buf_dispose(&buf_); }

    BufGuard(const BufGuard&) = delete;
    BufGuard& operator=(const BufGuard&) = delete;

    [[nodiscard]] git_buf* out() noexcept { return &buf_; }
    [[nodiscard]] std::string str() const {
        return buf_.ptr != nullptr ? std::string(buf_.ptr, buf_.size) : std::string{};
    }

private:
    git_buf buf_{};
};

void ensure_libgit2_initialized() {
    static const int initialized = git_libgit2_init();
    (void)initialized;
}

[[noreturn]] void throw_git_error(ToolErrorCode code, std::string what) {
    const git_error* error = git_error_last();
    if (error != nullptr && error->message != nullptr) {
        what += ": ";
        what += error->message;
    }
    throw ToolError{code, std::move(what)};
}

void check(int rc, std::string_view what) {
    if (rc < 0) {
        throw_git_error(ToolErrorCode::Internal, std::string(what));
    }
}

RepoPtr open_repository(const std::filesystem::path& path) {
    if (path.empty()) {
        throw ToolError{ToolErrorCode::InvalidArguments, "git path must not be empty"};
    }
    ensure_libgit2_initialized();
    git_repository* repo = nullptr;
    const std::string text = path.string();
    const int rc = git_repository_open_ext(&repo, text.c_str(), 0, nullptr);
    if (rc != 0) {
        throw ToolError{ToolErrorCode::NotFound, "not a git repository: " + text};
    }
    return RepoPtr(repo);
}

std::string branch_name(git_repository* repo) {
    git_reference* head = nullptr;
    const int rc = git_repository_head(&head, repo);
    if (rc == GIT_EUNBORNBRANCH) {
        return "No commits yet";
    }
    if (rc != 0) {
        return "HEAD";
    }
    RefPtr ref(head);
    const char* shorthand = git_reference_shorthand(head);
    return shorthand != nullptr ? shorthand : "HEAD";
}

char index_status_char(unsigned int status) {
    if ((status & GIT_STATUS_INDEX_NEW) != 0) return 'A';
    if ((status & GIT_STATUS_INDEX_MODIFIED) != 0) return 'M';
    if ((status & GIT_STATUS_INDEX_DELETED) != 0) return 'D';
    if ((status & GIT_STATUS_INDEX_RENAMED) != 0) return 'R';
    if ((status & GIT_STATUS_INDEX_TYPECHANGE) != 0) return 'T';
    return ' ';
}

char worktree_status_char(unsigned int status) {
    if ((status & GIT_STATUS_WT_NEW) != 0) return '?';
    if ((status & GIT_STATUS_WT_MODIFIED) != 0) return 'M';
    if ((status & GIT_STATUS_WT_DELETED) != 0) return 'D';
    if ((status & GIT_STATUS_WT_RENAMED) != 0) return 'R';
    if ((status & GIT_STATUS_WT_TYPECHANGE) != 0) return 'T';
    return ' ';
}

std::string status_code(unsigned int status) {
    if ((status & GIT_STATUS_CONFLICTED) != 0) {
        return "UU";
    }
    if ((status & GIT_STATUS_WT_NEW) != 0) {
        return "??";
    }
    std::string code;
    code.push_back(index_status_char(status));
    code.push_back(worktree_status_char(status));
    return code;
}

std::string status_path(const git_status_entry* entry) {
    if (entry->index_to_workdir != nullptr &&
        entry->index_to_workdir->new_file.path != nullptr) {
        return entry->index_to_workdir->new_file.path;
    }
    if (entry->head_to_index != nullptr &&
        entry->head_to_index->new_file.path != nullptr) {
        return entry->head_to_index->new_file.path;
    }
    return {};
}

TreePtr resolve_tree(git_repository* repo, const std::string& ref) {
    git_object* object = nullptr;
    if (git_revparse_single(&object, repo, ref.c_str()) != 0) {
        throw_git_error(ToolErrorCode::NotFound, "unknown git ref: " + ref);
    }
    ObjectPtr parsed(object);

    git_object* peeled = nullptr;
    check(git_object_peel(&peeled, parsed.get(), GIT_OBJECT_TREE), "git_object_peel");
    ObjectPtr peeled_guard(peeled);

    git_tree* tree = nullptr;
    check(git_tree_lookup(&tree, repo, git_object_id(peeled)), "git_tree_lookup");
    return TreePtr(tree);
}

TreePtr head_tree(git_repository* repo) {
    git_reference* head = nullptr;
    const int rc = git_repository_head(&head, repo);
    if (rc == GIT_EUNBORNBRANCH) {
        return TreePtr{};
    }
    check(rc, "git_repository_head");
    RefPtr ref(head);

    git_object* peeled = nullptr;
    check(git_reference_peel(&peeled, head, GIT_OBJECT_TREE), "git_reference_peel");
    ObjectPtr peeled_guard(peeled);

    git_tree* tree = nullptr;
    check(git_tree_lookup(&tree, repo, git_object_id(peeled)), "git_tree_lookup");
    return TreePtr(tree);
}

std::string relative_pathspec(git_repository* repo, const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    const char* workdir = git_repository_workdir(repo);
    if (workdir == nullptr) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(path, std::filesystem::path(workdir), ec);
    if (ec || relative.empty() || relative == ".") {
        return {};
    }
    return relative.generic_string();
}

DiffPtr create_diff(git_repository* repo, const GitQuery& query) {
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    std::string pathspec = relative_pathspec(repo, query.path);
    char* pathspec_data = pathspec.empty() ? nullptr : pathspec.data();
    if (pathspec_data != nullptr) {
        options.pathspec.strings = &pathspec_data;
        options.pathspec.count = 1;
    }

    git_index* raw_index = nullptr;
    check(git_repository_index(&raw_index, repo), "git_repository_index");
    IndexPtr index(raw_index);

    git_diff* result = nullptr;
    if (query.staged) {
        TreePtr tree = query.ref.empty() ? head_tree(repo) : resolve_tree(repo, query.ref);
        check(git_diff_tree_to_index(&result, repo, tree.get(), index.get(), &options),
              "git_diff_tree_to_index");
        return DiffPtr(result);
    }

    if (!query.ref.empty()) {
        TreePtr tree = resolve_tree(repo, query.ref);
        check(git_diff_tree_to_workdir(&result, repo, tree.get(), &options),
              "git_diff_tree_to_workdir");
        return DiffPtr(result);
    }

    check(git_diff_index_to_workdir(&result, repo, index.get(), &options),
          "git_diff_index_to_workdir");
    return DiffPtr(result);
}

[[noreturn]] void unavailable(std::string_view operation) {
    throw ToolError{ToolErrorCode::Internal,
                    "git " + std::string(operation) + " is not implemented (read-only MVP)"};
}

} // namespace

Task<GitStatus> LibGit2GitService::status(const GitQuery& query) {
    RepoPtr repo = open_repository(query.path);

    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                    GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

    git_status_list* raw_list = nullptr;
    check(git_status_list_new(&raw_list, repo.get(), &options), "git_status_list_new");
    StatusListPtr list(raw_list);

    std::vector<std::pair<std::string, std::string>> entries;
    const std::size_t count = git_status_list_entrycount(list.get());
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const git_status_entry* entry = git_status_byindex(list.get(), i);
        if (entry == nullptr || (entry->status & GIT_STATUS_IGNORED) != 0) {
            continue;
        }
        std::string path = status_path(entry);
        if (path.empty()) {
            continue;
        }
        entries.emplace_back(std::move(path), status_code(entry->status));
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::string text = "## " + branch_name(repo.get()) + "\n";
    for (const auto& [path, code] : entries) {
        text += code;
        text += ' ';
        text += path;
        text += '\n';
    }
    return Task<GitStatus>(GitStatus{std::move(text)});
}

Task<GitDiff> LibGit2GitService::diff(const GitQuery& query) {
    RepoPtr repo = open_repository(query.path);
    DiffPtr diff = create_diff(repo.get(), query);

    BufGuard buffer;
    check(git_diff_to_buf(buffer.out(), diff.get(), GIT_DIFF_FORMAT_PATCH),
          "git_diff_to_buf");
    return Task<GitDiff>(GitDiff{buffer.str()});
}

Task<GitLog> LibGit2GitService::log(const GitQuery&) { unavailable("log"); }

Task<GitShow> LibGit2GitService::show(const GitQuery&) { unavailable("show"); }

Task<GitBranch> LibGit2GitService::branch(const GitQuery&) { unavailable("branch"); }

Task<void> LibGit2GitService::checkout(const GitQuery&) { unavailable("checkout"); }

} // namespace ymh
