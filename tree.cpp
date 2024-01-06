#define _POSIX_C_SOURCE 200809L

#include <stdint.h>     /* uint64_t */
#include <dirent.h>     /* opendir, closedir */
#include <fcntl.h>      /* open */
#include <sys/types.h>  /* DIR */
#include <sys/stat.h>   /* fstatat */

#include <cerrno>       /* errno */
#include <iostream>
#include <vector>
#include <cstring>      /* strerror */
#include <string>
#include <iterator>
#include <stdexcept>    /* runtime_error */
#include <string_view>  /* sv */
#include <memory>       /* shared_ptr */
#include <optional>

using namespace std::literals;

auto sys_err(const char* str)
{
    int err = errno;
    return std::runtime_error(std::string(str) + ": ("
                    + std::to_string(err) + ") "
                    + std::strerror(err));
}

auto sys_errx(const char* str)
{
    return std::runtime_error(std::string(str));
}

struct fd_t
{
    int fd = -1;

    fd_t(int fd_) : fd(fd_) {}
    ~fd_t() { if (fd != -1 && fd != AT_FDCWD) ::close(fd); }

    fd_t(const fd_t&) = delete;
    fd_t& operator=(const fd_t&) = delete;

    fd_t(fd_t&&) noexcept = default;
    fd_t& operator=(fd_t&&) noexcept = default;
};

struct dir_t
{
    DIR* dir;

    dir_t(DIR* dir_) : dir(dir_) {}
    ~dir_t() { if (dir) ::closedir(dir); }

    dir_t(const dir_t&) = delete;
    dir_t& operator=(const dir_t&) = delete;

    dir_t(dir_t&&) noexcept = default;
    dir_t& operator=(dir_t&&) noexcept = default;
};

using shared_fd = std::shared_ptr<fd_t>;
using shared_dir = std::shared_ptr<dir_t>;

struct iter_t;

struct file_t
{
    shared_fd at = nullptr;
    ::mode_t mode = 0;

    std::string name;

    file_t(shared_fd at_, const std::string& name)
        : at(at_), name(name)
    {
        struct stat st;
        if (::fstatat(at->fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1)
            throw sys_err("fstatat");
        mode = st.st_mode;
    }

    static DIR* make_dir(int at, const std::string& name)
    {
        int fd = ::openat(at, name.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd == -1)
            throw sys_err("openat");

        DIR* dir = ::fdopendir(fd);
        if (!dir)
        {
            ::close(fd);
            throw sys_err("fdopendir");
        }
        return dir;
    }

    friend std::ostream& operator<<(std::ostream& o, const file_t& f)
    {
        return o << f.name;
    }

    bool is_dir() const { return S_ISDIR(mode); }

    iter_t begin() const;
    iter_t end() const;
};

struct iter_t
{
    shared_fd at = nullptr;
    shared_dir dir = nullptr;
    std::optional<std::string> d_name = std::nullopt;

    iter_t() = default;

    iter_t(shared_fd at_, shared_dir dir_)
        : at(at_),
          dir(dir_)
    {
        ++(*this);
    }

    file_t operator*() const
    {
        auto copy = std::make_shared<fd_t>(::dup(::dirfd(dir->dir)));
        return file_t(copy, d_name.value());
    }

    iter_t& operator++()
    {
        if (!dir)
            throw sys_errx("++ on null dir");

        ::dirent* entry;
        do
        {
            errno = 0;
            entry = ::readdir(dir->dir);

        } while (entry && ( entry->d_name == "."sv
                         || entry->d_name == ".."sv ));

        if (!entry && errno != 0)
            throw sys_err("readdir");

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
    return iter_t(at, std::make_shared<dir_t>(make_dir(at->fd, name)));
}
iter_t file_t::end() const { return iter_t(); }

template<typename Node>
void print_rec(const Node& node, std::vector<bool>& lines,
               decltype(std::cout)& out, bool first, bool last)
{
    for (int l : lines)
        out << (l ? "│   " : "    ");

    if (!first) out << (last ? "└── " : "├── ");
    out << node << "\n";

    if (!first) lines.push_back(!last);

    auto end = node.end();
    decltype(end) next;
    for (auto it = node.begin(); it != end; it = next)
    {
        next = it;
        ++next;
        print_rec(*it, lines, out, false, next == end);
    }

    if (!first) lines.pop_back();
}

template<typename Node>
void print(const Node& node, decltype(std::cout)& out = std::cout)
{
    std::vector<bool> lines;
    print_rec(node, lines, out, true, true);
}

void run(const file_t& file)
{
    std::cout << file.name << "\n";
    if (file.is_dir())
    {
        for (const file_t& f : file)
            run(f);
    }
}

int main(int argc, char** argv)
{
    auto show = [&](const auto path)
    {
        auto f = file_t(std::make_shared<fd_t>(AT_FDCWD), path);
        print(f);
    };

    if (argc < 2)
        return show("."), 0;

    for (int i = 1; i < argc; ++i)
    {
        if (i > 1) std::cout << "\n";
        show(argv[i]);
    }

    return 0;
}
