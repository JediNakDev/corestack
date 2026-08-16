#include "../system_program.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

/*
    sys: print basic information about the operating system,
    in the spirit of neofetch. Reports the OS / kernel, total
    memory, the logged-in user, and the CPU. Works on both
    macOS (development) and Linux (grading) hosts.
*/

/* Read the total physical memory of the machine, in bytes.
   Returns 0 if the value could not be determined. */
static unsigned long long get_total_memory(void)
{
#ifdef __APPLE__
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    unsigned long long memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctl(mib, 2, &memsize, &len, NULL, 0) == 0)
    {
        return memsize;
    }
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
    {
        return (unsigned long long)pages * (unsigned long long)page_size;
    }
    return 0;
#endif
}

/* Fill "buf" with a human readable CPU model name. Falls back to
   the machine architecture from uname() if no model is available. */
static void get_cpu_model(char *buf, size_t size, const char *fallback)
{
#ifdef __APPLE__
    if (sysctlbyname("machdep.cpu.brand_string", buf, &size, NULL, 0) == 0)
    {
        return;
    }
#else
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f)
    {
        char line[SHELL_BUFFERSIZE];
        while (fgets(line, sizeof(line), f))
        {
            if (strncmp(line, "model name", 10) == 0)
            {
                char *colon = strchr(line, ':');
                if (colon)
                {
                    colon++;
                    while (*colon == ' ' || *colon == '\t')
                        colon++;
                    colon[strcspn(colon, "\n")] = '\0';
                    strncpy(buf, colon, size - 1);
                    buf[size - 1] = '\0';
                    fclose(f);
                    return;
                }
            }
        }
        fclose(f);
    }
#endif
    /* Fall back to the architecture reported by uname(). */
    strncpy(buf, fallback, size - 1);
    buf[size - 1] = '\0';
}

/* Count the number of online (available) logical CPU cores. */
static long get_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? n : 1;
}

int execute(char **args)
{
    (void)args;

    struct utsname sys_info;
    if (uname(&sys_info) != 0)
    {
        perror("uname failed");
        return EXIT_FAILURE;
    }

    /* Resolve the currently logged-in user. Prefer the password
       database entry, then fall back to getlogin()/$USER. */
    const char *username = NULL;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_name)
    {
        username = pw->pw_name;
    }
    if (!username)
    {
        username = getlogin();
    }
    if (!username)
    {
        username = getenv("USER");
    }
    if (!username)
    {
        username = "unknown";
    }

    char hostname[SHELL_BUFFERSIZE];
    if (gethostname(hostname, sizeof(hostname)) != 0)
    {
        strncpy(hostname, sys_info.nodename, sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }

    char cpu_model[SHELL_BUFFERSIZE];
    get_cpu_model(cpu_model, sizeof(cpu_model), sys_info.machine);

    unsigned long long total_mem = get_total_memory();
    double total_mem_gib = (double)total_mem / (1024.0 * 1024.0 * 1024.0);

    /* user@host header, neofetch style. */
    printf(COLOR_GREEN "%s" COLOR_RESET "@" COLOR_GREEN "%s\n" COLOR_RESET,
           username, hostname);
    printf("-----------------------------\n");

    printf(COLOR_CYAN "OS:      " COLOR_RESET "%s\n", sys_info.sysname);
    printf(COLOR_CYAN "Host:    " COLOR_RESET "%s\n", sys_info.nodename);
    printf(COLOR_CYAN "Kernel:  " COLOR_RESET "%s %s\n", sys_info.release,
           sys_info.machine);
    printf(COLOR_CYAN "User:    " COLOR_RESET "%s\n", username);
    printf(COLOR_CYAN "CPU:     " COLOR_RESET "%s (%ld cores)\n", cpu_model,
           get_cpu_count());
    if (total_mem > 0)
    {
        printf(COLOR_CYAN "Memory:  " COLOR_RESET "%.2f GiB (%llu bytes)\n",
               total_mem_gib, total_mem);
    }
    else
    {
        printf(COLOR_CYAN "Memory:  " COLOR_RESET "unavailable\n");
    }

    return EXIT_SUCCESS;
}

int main(int argc, char **args)
{
    (void)argc;
    return execute(args);
}
