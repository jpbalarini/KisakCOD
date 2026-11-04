// kvfs.h -- virtual file layer under the Win32 file shim.
//
// Two backends, selected at runtime:
//   * POSIX  (node / M0): real files, with a case-insensitive path resolver (retail data was authored for NTFS).
//   * HTTP   (browser):   the install directory is read through HTTP range requests against the local dev server
//                         (/data/<path>, see web/tools/serve.py) using synchronous XHR from the calling worker thread, so
//                         every engine thread (main, fastfile loader) can read files without any cross-thread proxying.
//                         Files the engine writes (configs, profiles) live in memory.
// All paths are forward-slash, as normalised by the Win32 shim.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <sys/types.h>

namespace kvfs {

struct Entry { std::string name; bool isDir; uint64_t size; };

// Choose the backend and (browser) download the file manifest. `installRoot` is the path the engine believes the game is
// installed at; paths below it are served remotely. Safe to call more than once.
void init(const char *installRoot);
bool isBrowser();

int      open(const std::string &path, int oflags);   // POSIX O_* flags; returns a handle >= 0 or -1 (errno set)
int      close(int h);
int64_t  pread(int h, void *buf, size_t n, int64_t off);
int64_t  read(int h, void *buf, size_t n);
int64_t  write(int h, const void *buf, size_t n);
int64_t  lseek(int h, int64_t off, int whence);
bool     fileSize(int h, int64_t *size);
bool     statPath(const std::string &path, bool *isDir, int64_t *size, std::string *canonical = nullptr);
bool     listdir(const std::string &dir, std::vector<Entry> *out);
bool     unlink(const std::string &path);
bool     mkdir(const std::string &path);
bool     rmdir(const std::string &path);

} // namespace kvfs
