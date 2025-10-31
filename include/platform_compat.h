#pragma once

// Platform compatibility layer for Windows/UNIX portability

// Packed struct macros
#ifdef _WIN32
    #define PACK_START __pragma(pack(push, 1))
    #define PACK_END __pragma(pack(pop))
    #define PACKED
#else
    #define PACK_START
    #define PACK_END
    #define PACKED __attribute__((packed))
#endif

#ifdef _WIN32
    // Windows-specific includes
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX  // Prevent min/max macro conflicts
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/stat.h>
    #include <process.h>

    // Undefine Windows macros that conflict with our method names
    #ifdef SetCurrentDirectory
        #undef SetCurrentDirectory
    #endif

    // UNIX-to-Windows type mappings
    #define ssize_t SSIZE_T

    // Socket compatibility
    typedef SOCKET socket_t;
    #define close closesocket
    #define ioctl ioctlsocket

    // Socket address families (if not already defined)
    #ifndef AF_UNIX
        #define AF_UNIX AF_UNSPEC  // Unix domain sockets not supported on Windows
    #endif

    // UNIX domain socket structure (stub for Windows)
    struct sockaddr_un {
        unsigned short sun_family;
        char sun_path[108];
    };

    // File descriptor operations
    #define F_GETFL 0
    #define F_SETFL 1
    #define F_OK 0
    #define O_NONBLOCK 1
    #define O_RDONLY _O_RDONLY
    #define O_RDWR _O_RDWR
    #define O_WRONLY _O_WRONLY

    // File type macros
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
    #define S_ISCHR(m) (((m) & _S_IFMT) == _S_IFCHR)

    // Process operations
    #define popen _popen
    #define pclose _pclose

    // Socket flags
    #ifndef MSG_DONTWAIT
        #define MSG_DONTWAIT 0  // Windows doesn't have MSG_DONTWAIT, use non-blocking socket instead
    #endif

    // Memory mapping stubs (Windows doesn't use mmap the same way)
    #define PROT_READ   0x1
    #define PROT_WRITE  0x2
    #define MAP_SHARED  0x1
    #define MAP_PRIVATE 0x2
    #define MAP_FAILED  ((void*)-1)

    // Memory mapping and advise
    #define MADV_RANDOM 0  // Not used on Windows

    // mmap/munmap stubs (not implemented, will return error)
    inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
        return MAP_FAILED;  // Not implemented on Windows
    }
    inline int munmap(void* addr, size_t length) {
        return -1;  // Not implemented on Windows
    }
    inline int madvise(void* addr, size_t length, int advice) {
        return 0;  // Stub on Windows
    }

    // unistd.h functions
    inline int fcntl(int fd, int cmd, ...) { return -1; }  // Stub
    inline unsigned int sleep(unsigned int seconds) {
        Sleep(seconds * 1000);
        return 0;
    }
    inline void usleep(unsigned int microseconds) {
        Sleep(microseconds / 1000);  // Sleep takes milliseconds
    }
    inline int access(const char* path, int mode) {
        return _access(path, mode);
    }

    // OpenGL constants missing in Windows headers
    #ifndef GL_CLAMP_TO_EDGE
        #define GL_CLAMP_TO_EDGE 0x812F
    #endif

    // POSIX memmem implementation for Windows
    inline void* memmem(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen) {
        if (needlelen == 0) return (void*)haystack;
        const char* h = (const char*)haystack;
        const char* n = (const char*)needle;
        for (size_t i = 0; i <= haystacklen - needlelen; i++) {
            if (memcmp(&h[i], n, needlelen) == 0) {
                return (void*)&h[i];
            }
        }
        return nullptr;
    }

    // Byte swap functions (GCC __builtin_bswap32 equivalent)
    #include <stdlib.h>
    #define __builtin_bswap16(x) _byteswap_ushort(x)
    #define __builtin_bswap32(x) _byteswap_ulong(x)
    #define __builtin_bswap64(x) _byteswap_uint64(x)

    // User/group ID stubs (not used on Windows)
    typedef int uid_t;
    typedef int gid_t;
    inline uid_t getuid() { return 0; }
    inline gid_t getgid() { return 0; }

    // Password database stub (pwd.h)
    struct passwd {
        char* pw_name;
        char* pw_dir;
        uid_t pw_uid;
    };
    inline struct passwd* getpwuid(uid_t uid) {
        static struct passwd pw;
        static char homedir[260];
        char* userprofile = getenv("USERPROFILE");
        if (userprofile) {
            strncpy_s(homedir, sizeof(homedir), userprofile, _TRUNCATE);
        } else {
            strcpy_s(homedir, sizeof(homedir), "C:\\");
        }
        pw.pw_dir = homedir;
        pw.pw_name = getenv("USERNAME");
        pw.pw_uid = 0;
        return &pw;
    }

#else
    // UNIX/Linux includes
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <sys/mman.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <fcntl.h>

    typedef int socket_t;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif
