#include "../userabi.h"
long user_main(void) {
#if defined(ANIX_SHELL_BOOT)
    anix_puts("anix init: starting interactive shell\n");
    if (anix_execve("/bin/anixsh") < 0) anix_puts("anix init: shell exec failed\n");
    return 1;
#else
    anix_puts("anix init: starting /bin/sdk-hello\n");
    char *const app_argv[] = { "/bin/sdk-hello", "from-init", 0 };
#if defined(ANIX_ENABLE_STDIN_TEST)
    char *const app_envp[] = { "ANIX_INIT_ENV=present", "ANIX_TEST_STDIN=1", 0 };
#else
    char *const app_envp[] = { "ANIX_INIT_ENV=present", 0 };
#endif
    if (anix_execv("/bin/sdk-hello", app_argv, app_envp) < 0)
        anix_puts("anix init: SDK demo failed; starting shell\n");
    if (anix_execve("/bin/anixsh") < 0) anix_puts("anix init: shell exec failed\n");
    return 1;
#endif
}
