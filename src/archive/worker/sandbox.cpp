#include "worker.h"
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace nook::archive::worker {

bool restrictWrites(const QString &writableDir) {
    const long abi = syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1)
        return false;
    __u64 handled = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                    LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
                    LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
                    LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                    LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
    landlock_ruleset_attr attributes{};
    attributes.handled_access_fs = handled;
    const int ruleset = int(syscall(SYS_landlock_create_ruleset, &attributes, sizeof attributes, 0));
    if (ruleset < 0)
        return false;
    bool ok = true;
    if (!writableDir.isEmpty()) {
        const int parent = ::open(writableDir.toLocal8Bit().constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        landlock_path_beneath_attr rule{};
        // Device nodes, sockets and FIFOs never come out of a package.
        rule.allowed_access = handled & ~(LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                                          LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO);
        rule.parent_fd = parent;
        ok = parent >= 0 && syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
        if (parent >= 0)
            ::close(parent);
    }
    ok = ok && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         syscall(SYS_landlock_restrict_self, ruleset, 0) == 0;
    ::close(ruleset);
    return ok;
}

}
