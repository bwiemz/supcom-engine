#include "platform/steam_library.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace osc::platform {

namespace fs = std::filesystem;

namespace {

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Recursive-descent parser over VDF tokens: quoted strings (with \\ \"
/// \n \t escapes), unquoted words, braces, and // line comments.
class VdfParser {
public:
    explicit VdfParser(std::string_view text) : text_(text) {}

    std::optional<VdfNode> parse_root() {
        VdfNode root;
        root.is_object = true;
        if (!parse_members(root, /*top_level=*/true)) return std::nullopt;
        return root;
    }

private:
    enum class Tok { String, Open, Close, End, Error };

    std::string_view text_;
    size_t pos_ = 0;
    std::string tok_text_;

    void skip_space_and_comments() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    Tok next() {
        skip_space_and_comments();
        if (pos_ >= text_.size()) return Tok::End;
        char c = text_[pos_];
        if (c == '{') { ++pos_; return Tok::Open; }
        if (c == '}') { ++pos_; return Tok::Close; }
        tok_text_.clear();
        if (c == '"') {
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] != '"') {
                char ch = text_[pos_++];
                if (ch == '\\' && pos_ < text_.size()) {
                    char esc = text_[pos_++];
                    switch (esc) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    default: ch = esc; break; // \\ and \" (and anything else)
                    }
                }
                tok_text_ += ch;
            }
            if (pos_ >= text_.size()) return Tok::Error; // unterminated
            ++pos_;
            return Tok::String;
        }
        while (pos_ < text_.size() &&
               !std::isspace(static_cast<unsigned char>(text_[pos_])) &&
               text_[pos_] != '{' && text_[pos_] != '}' && text_[pos_] != '"') {
            tok_text_ += text_[pos_++];
        }
        return Tok::String;
    }

    bool parse_members(VdfNode& parent, bool top_level) {
        for (;;) {
            Tok t = next();
            if (t == Tok::End) return top_level;
            if (t == Tok::Close) return !top_level;
            if (t != Tok::String) return false;
            VdfNode node;
            node.key = tok_text_;
            Tok v = next();
            if (v == Tok::String) {
                node.value = tok_text_;
            } else if (v == Tok::Open) {
                node.is_object = true;
                if (!parse_members(node, /*top_level=*/false)) return false;
            } else {
                return false;
            }
            parent.children.push_back(std::move(node));
        }
    }
};

void add_existing_unique(std::vector<fs::path>& out, const fs::path& candidate) {
    std::error_code ec;
    if (!fs::is_directory(candidate, ec)) return;
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    for (const auto& existing : out) {
        if (fs::weakly_canonical(existing, ec) == canonical) return;
    }
    out.push_back(candidate);
}

} // namespace

const VdfNode* VdfNode::child(std::string_view name) const {
    for (const auto& c : children) {
        if (iequals(c.key, name)) return &c;
    }
    return nullptr;
}

std::string VdfNode::value_of(std::string_view name) const {
    const VdfNode* c = child(name);
    return (c && !c->is_object) ? c->value : std::string();
}

std::optional<VdfNode> parse_vdf(std::string_view text) {
    return VdfParser(text).parse_root();
}

std::vector<fs::path> parse_library_folders_vdf(std::string_view text) {
    std::vector<fs::path> out;
    auto root = parse_vdf(text);
    if (!root) return out;
    const VdfNode* folders = root->child("libraryfolders");
    if (!folders) return out;
    for (const auto& entry : folders->children) {
        if (entry.is_object) {
            std::string path = entry.value_of("path");
            if (!path.empty()) out.emplace_back(path);
        } else if (!entry.key.empty() &&
                   std::all_of(entry.key.begin(), entry.key.end(), [](char c) {
                       return std::isdigit(static_cast<unsigned char>(c));
                   })) {
            out.emplace_back(entry.value); // legacy: "1" "D:\\SteamLibrary"
        }
    }
    return out;
}

std::optional<std::string> parse_app_install_dir(std::string_view acf_text) {
    auto root = parse_vdf(acf_text);
    if (!root) return std::nullopt;
    const VdfNode* state = root->child("AppState");
    if (!state) return std::nullopt;
    std::string dir = state->value_of("installdir");
    if (dir.empty()) return std::nullopt;
    return dir;
}

std::vector<fs::path> steam_roots(const EnvLookup& env) {
    std::vector<fs::path> roots;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        add_existing_unique(roots, fs::path(buf));
    }
    if (auto pf86 = env("ProgramFiles(x86)")) {
        add_existing_unique(roots, fs::path(*pf86) / "Steam");
    }
    add_existing_unique(roots, fs::path("C:/Program Files (x86)/Steam"));
#else
    const fs::path home = home_dir(env);
    add_existing_unique(roots, known_folder(KnownFolder::LocalAppData, env) / "Steam");
    add_existing_unique(roots, home / ".local" / "share" / "Steam");
    add_existing_unique(roots, home / ".steam" / "steam");
    add_existing_unique(roots, home / ".steam" / "root");
    // Flatpak Steam keeps its own XDG tree.
    add_existing_unique(roots, home / ".var" / "app" / "com.valvesoftware.Steam" /
                                   ".local" / "share" / "Steam");
#endif
    return roots;
}

std::optional<fs::path> find_steam_app(std::uint32_t app_id,
                                       const std::vector<fs::path>& roots) {
    const std::string manifest = "appmanifest_" + std::to_string(app_id) + ".acf";
    for (const auto& root : roots) {
        std::vector<fs::path> libraries{root};
        for (const auto& vdf : {root / "steamapps" / "libraryfolders.vdf",
                                root / "config" / "libraryfolders.vdf"}) {
            if (auto text = read_text(vdf)) {
                for (auto& lib : parse_library_folders_vdf(*text)) {
                    libraries.push_back(std::move(lib));
                }
            }
        }
        for (const auto& lib : libraries) {
            auto acf = read_text(lib / "steamapps" / manifest);
            if (!acf) continue;
            auto install_dir = parse_app_install_dir(*acf);
            if (!install_dir) continue;
            fs::path candidate = lib / "steamapps" / "common" / *install_dir;
            std::error_code ec;
            if (fs::is_directory(candidate, ec)) return candidate;
        }
    }
    return std::nullopt;
}

} // namespace osc::platform
