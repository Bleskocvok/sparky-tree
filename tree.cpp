#define _POSIX_C_SOURCE 200809L

#include <stdint.h>     /* uint64_t */
#include <dirent.h>     /* opendir, closedir */
#include <fcntl.h>      /* open */
#include <sys/types.h>  /* DIR */
#include <sys/stat.h>   /* fstatat */
#include <unistd.h>     /* isatty */
#include <unistd.h>     /* getopt */

#include <cerrno>       /* errno */
#include <cstring>      /* strerror */
#include <stdexcept>    /* runtime_error */
#include <string_view>  /* sv */
#include <memory>       /* shared_ptr */
#include <optional>     /* nullopt */
#include <utility>      /* exchange */
#include <iostream>
#include <vector>
#include <string>       /* string, stoi */
#include <iterator>
#include <variant>

using namespace std::literals;

auto sys_errx(const char* str)
{
    return std::runtime_error(std::string(str));
}

std::string sys_err_str(const char* str)
{
    int err = errno;
    return std::string(str) + ": ("
         + std::to_string(err) + ") "
         + std::strerror(err);
}

inline bool OutputTerminal = false;

struct err_t
{
    std::string str;

    err_t(std::string&& str) : str(str) {}

    friend std::ostream& operator<<(std::ostream& o, const err_t& e)
    {
        return o
            << ( OutputTerminal ? "\e[1;31m" : "" )
            << "(error: " << e.str << ")"
            << ( OutputTerminal ? "\e[0m" : "" );
    }
};

struct fd_t
{
    int fd = -1;

    fd_t(int fd_) : fd(fd_) {}
    ~fd_t() { close(); }

    fd_t(const fd_t&) = delete;
    fd_t& operator=(const fd_t&) = delete;

    fd_t(fd_t&& o) noexcept
        : fd(std::exchange(o.fd, -1))
    { }

    fd_t& operator=(fd_t&& o) noexcept
    {
        close();
        fd = std::exchange(o.fd, -1);
        return *this;
    }

    void close()
    {
        if (fd != -1 && fd != AT_FDCWD)
            ::close(std::exchange(fd, -1));
    }
};

struct dir_t
{
    DIR* dir = nullptr;

    dir_t(DIR* dir_) : dir(dir_) {}

    ~dir_t() { close(); }

    dir_t(const dir_t&) = delete;
    dir_t& operator=(const dir_t&) = delete;

    dir_t(dir_t&& other) noexcept
        : dir(std::exchange(other.dir, nullptr))
    { }

    dir_t& operator=(dir_t&& other) noexcept
    {
        close();
        dir = std::exchange(other.dir, nullptr);
        return *this;
    }

    void close()
    {
        if (dir) ::closedir(std::exchange(dir, nullptr));
    }
};

using shared_fd = std::shared_ptr<fd_t>;
using shared_dir = std::shared_ptr<dir_t>;

using maybe_err = std::optional<err_t>;

struct iter_t;

struct directory_t
{
    shared_dir dir = nullptr;
    maybe_err error = {};
};

struct file_t
{
    shared_fd at = nullptr;
    ::mode_t mode = 0;
    maybe_err err_ = {};

    std::string name;

    file_t(shared_fd at_, const std::string& name)
        : at(at_), name(name)
    {
        struct stat st;
        if (::fstatat(at->fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1)
            err_ = sys_err_str("fstatat");
        else
            mode = st.st_mode;
    }

    const auto& error() const { return err_; }

    static directory_t open_as_dir(int at, const std::string& name)
    {
        int fd = ::openat(at, name.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd == -1)
            return { nullptr, sys_err_str("openat") };

        DIR* dir = ::fdopendir(fd);
        if (!dir)
        {
            ::close(fd);
            return { nullptr, sys_err_str("fdopendir") };
        }
        return { std::make_shared<dir_t>(dir) };
    }

    friend std::ostream& operator<<(std::ostream& o, const file_t& f)
    {
        o << f.name;
        if (f.error()) o << " " << *f.error();
        return o;
    }

    bool is_dir() const { return !err_ && S_ISDIR(mode); }

    iter_t begin() const;
    iter_t end() const;
};

struct iter_t
{
    shared_fd at = nullptr;
    directory_t dir;
    std::optional<std::string> d_name = std::nullopt;
    maybe_err err_ = {};

    iter_t() = default;

    iter_t(shared_fd at_, directory_t dir_)
        : at(at_),
          dir(dir_),
          err_(dir.error)
    {
        if (!err_) ++(*this);
    }

    const auto& error() const { return dir.error; }

    file_t operator*() const
    {
        // omg
        auto copy = std::make_shared<fd_t>(::dup(::dirfd(dir.dir->dir)));
        return file_t(copy, d_name.value());
    }

    iter_t& operator++()
    {
        if (!dir.dir)
            throw sys_errx("++ on null dir");

        ::dirent* entry;
        do
        {
            errno = 0;
            entry = ::readdir(dir.dir->dir);

        } while (entry && ( entry->d_name == "."sv
                         || entry->d_name == ".."sv ));

        if (!entry && errno != 0)
        {
            err_ = sys_err_str("readdir");
            return *this;
        }

        d_name = entry ? decltype(d_name){ entry->d_name } : std::nullopt;

        return *this;
    }

    friend bool operator==(const iter_t& a, const iter_t& b)
    {
        return a.d_name == b.d_name;
    }
};

iter_t file_t::begin() const
{
    if (!is_dir()) return end();
    return iter_t(at, open_as_dir(at->fd, name));
}
iter_t file_t::end() const { return iter_t(); }

template<typename Node>
void print_rec(const Node& node, std::vector<bool>& lines,
               decltype(std::cout)& out, bool first, bool last,
               int depth = -1)
{
    if (depth == 0) return;
    if (depth == -1) depth = 0;
    --depth;

    for (int l : lines)
        out << (l ? "│   " : "    ");

    if (!first) out << (last ? "└── " : "├── ");
    out << node;

    if (!first) lines.push_back(!last);

    if constexpr (requires{ node.begin(); node.end(); })
    {
        auto end = node.end();
        decltype(end) next;
        auto it = node.begin();

        if (it.error())
            out << " " << *std::exchange(it, end).error();

        out << "\n";

        for (; it != end && !it.error(); it = next)
        {
            next = it;
            ++next;
            print_rec(*it, lines, out, false, next == end, depth);
        }

        if (it.error())
            print_rec(*it.error(), lines, out, false, next == end, depth);
    }

    if (!first) lines.pop_back();
}

template<typename Node>
void print(const Node& node, decltype(std::cout)& out = std::cout,
           int depth = -1)
{
    std::vector<bool> lines;
    print_rec(node, lines, out, true, true, depth);
}

void usage(char** argv)
{
    fprintf(stderr, "usage: %s [-h] [-d depth] [DIR...]\n", argv[0]);
}

int main(int argc, char** argv)
{
    int depth = -1;

    auto show = [&](const auto path)
    {
        auto f = file_t(std::make_shared<fd_t>(AT_FDCWD), path);
        print(f, std::cout, depth);
    };

    OutputTerminal = bool(isatty(1));

    const char* optstr = "hd:";
    char c;
    while ((c = getopt(argc, argv, optstr)) != -1)
    {
        switch (c)
        {
            case 'h': return usage(argv), 0;
            case 'd': depth = std::stoi(optarg); break;
            default: return usage(argv), 1;
        }
    }
    argc -= optind - 1;
    argv += optind - 1;

    if (depth != -1) ++depth;

    if (argc < 2)
        return show("."), 0;

    for (int i = 1; i < argc; ++i)
    {
        if (i > 1) std::cout << "\n";
        show(argv[i]);
    }

    return 0;
}
