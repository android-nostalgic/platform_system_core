// Copyright 2006 The Android Open Source Project

#include <utils/misc.h>
#include <utils/logger.h>
#include <cutils/logd.h>
#include <cutils/sockets.h>
#include <cutils/logprint.h>
#include <cutils/event_tag_map.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define DEFAULT_LOG_ROTATE_SIZE_KBYTES 16
#define DEFAULT_MAX_ROTATED_LOGS 4

static AndroidLogFormat * g_logformat;

/* logd prefixes records with a length field */
#define RECORD_LENGTH_FIELD_SIZE_BYTES sizeof(uint32_t)

#define LOG_FILE_DIR    "/dev/log/"


namespace android {

/* Global Variables */

static const char * g_outputFileName = NULL;
static int g_logRotateSizeKBytes = 0;                   // 0 means "no log rotation"
static int g_maxRotatedLogs = DEFAULT_MAX_ROTATED_LOGS; // 0 means "unbounded"
static int g_outFD = -1;
static off_t g_outByteCount = 0;
static int g_isBinary = 0;
static int g_printBinary = 0;

static EventTagMap* g_eventTagMap = NULL;

static int openLogFile (const char *pathname)
{
    return open(g_outputFileName, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
}

static void rotateLogs()
{
    int err;

    // Can't rotate logs if we're not outputting to a file
    if (g_outputFileName == NULL) {
        return;
    }

    close(g_outFD);

    for (int i = g_maxRotatedLogs ; i > 0 ; i--) {
        char *file0, *file1;

        asprintf(&file1, "%s.%d", g_outputFileName, i);

        if (i - 1 == 0) {
            asprintf(&file0, "%s", g_outputFileName);
        } else {
            asprintf(&file0, "%s.%d", g_outputFileName, i - 1);
        }

        err = rename (file0, file1);

        if (err < 0 && errno != ENOENT) {
            perror("while rotating log files");
        }

        free(file1);
        free(file0);
    }

    g_outFD = openLogFile (g_outputFileName);

    if (g_outFD < 0) {
        perror ("couldn't open output file");
        exit(-1);
    }

    g_outByteCount = 0;

}

void printBinary(struct logger_entry *buf)
{
    size_t size = sizeof(logger_entry) + buf->len;
    int ret;
    
    do {
        ret = write(g_outFD, buf, size);
    } while (ret < 0 && errno == EINTR);
}

static void processBuffer(struct logger_entry *buf)
{
    int bytesWritten;
    int err;
    AndroidLogEntry entry;
    char binaryMsgBuf[1024];

    if (g_isBinary) {
        err = android_log_processBinaryLogBuffer(buf, &entry, g_eventTagMap,
                binaryMsgBuf, sizeof(binaryMsgBuf));
        //printf(">>> pri=%d len=%d msg='%s'\n",
        //    entry.priority, entry.messageLen, entry.message);
    } else {
        err = android_log_processLogBuffer(buf, &entry);
    }
    if (err < 0)
        goto error;

    bytesWritten = android_log_filterAndPrintLogLine(
                        g_logformat, g_outFD, &entry);

    if (bytesWritten < 0) {
        perror("output error");
        exit(-1);
    }

    g_outByteCount += bytesWritten;

    if (g_logRotateSizeKBytes > 0 
        && (g_outByteCount / 1024) >= g_logRotateSizeKBytes
    ) {
        rotateLogs();
    }

error:
    //fprintf (stderr, "Error processing record\n");
    return;
}

static void readLogLines(int logfd)
{
    while (1) {
        unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1] __attribute__((aligned(4)));
        struct logger_entry *entry = (struct logger_entry *) buf;
        int ret;

        ret = read(logfd, entry, LOGGER_ENTRY_MAX_LEN);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            perror("logcat read");
            exit(EXIT_FAILURE);
        }
        else if (!ret) {
            fprintf(stderr, "read: Unexpected EOF!\n");
            exit(EXIT_FAILURE);
        }

        /* NOTE: driver guarantees we read exactly one full entry */

        entry->msg[entry->len] = '\0';

        if (g_printBinary) {
            printBinary(entry);
        } else {
            (void) processBuffer(entry);
        }
    }
}

static int clearLog(int logfd)
{
    return ioctl(logfd, LOGGER_FLUSH_LOG);
}

/* returns the total size of the log's ring buffer */
static int getLogSize(int logfd)
{
    return ioctl(logfd, LOGGER_GET_LOG_BUF_SIZE);
}

/* returns the readable size of the log's ring buffer (that is, amount of the log consumed) */
static int getLogReadableSize(int logfd)
{
    return ioctl(logfd, LOGGER_GET_LOG_LEN);
}

static void setupOutput()
{

    if (g_outputFileName == NULL) {
        g_outFD = STDOUT_FILENO;

    } else {
        struct stat statbuf;

        g_outFD = openLogFile (g_outputFileName);

        if (g_outFD < 0) {
            perror ("couldn't open output file");
            exit(-1);
        }

        fstat(g_outFD, &statbuf);

        g_outByteCount = statbuf.st_size;
    }
}

static void show_help(const char *cmd)
{
    fprintf(stderr,"Usage: %s [options] [filterspecs]\n", cmd);

    fprintf(stderr, "options include:\n"
                    "  -s              Set default filter to silent.\n"
                    "                  Like specifying filterspec '*:s'\n"
                    "  -f <filename>   Log to file. Default to stdout\n"
                    "  -r [<kbytes>]   Rotate log every kbytes. (16 if unspecified). Requires -f\n"
                    "  -n <count>      Sets max number of rotated logs to <count>, default 4\n"
                    "  -v <format>     Sets the log print format, where <format> is one of:\n\n"
                    "                  brief process tag thread raw time threadtime long\n\n"
                    "  -c              clear (flush) the entire log and exit\n"
                    "  -d              dump the log and then exit (don't block)\n"
                    "  -g              get the size of the log's ring buffer and exit\n"
                    "  -b <buffer>     request alternate ring buffer\n"
                    "                  ('main' (default), 'radio', 'events')\n"
                    "  -B              output the log in binary");


    fprintf(stderr,"\nfilterspecs are a series of \n"
                   "  <tag>[:priority]\n\n"
                   "where <tag> is a log component tag (or * for all) and priority is:\n"
                   "  V    Verbose\n"
                   "  D    Debug\n"
                   "  I    Info\n"
                   "  W    Warn\n"
                   "  E    Error\n"
                   "  F    Fatal\n"
                   "  S    Silent (supress all output)\n"
                   "\n'*' means '*:d' and <tag> by itself means <tag>:v\n"
                   "\nIf not specified on the commandline, filterspec is set from ANDROID_LOG_TAGS.\n"
                   "If no filterspec is found, filter defaults to '*:I'\n"
                   "\nIf not specified with -v, format is set from ANDROID_PRINTF_LOG\n"
                   "or defaults to \"brief\"\n\n");



}


} /* namespace android */

static int setLogFormat(const char * formatString)
{
    static AndroidLogPrintFormat format;

    format = android_log_formatFromString(formatString);

    if (format == FORMAT_OFF) {
        // FORMAT_OFF means invalid string
        return -1;
    }

    android_log_setPrintFormat(g_logformat, format);

    return 0;
}

extern "C" void logprint_run_tests(void);

int main (int argc, char **argv)
{
    int logfd;
    int err;
    int hasSetLogFormat = 0;
    int clearLog = 0;
    int getLogSize = 0;
    int mode = O_RDONLY;
    char *log_device = strdup("/dev/"LOGGER_LOG_MAIN);
    const char *forceFilters = NULL;

    g_logformat = android_log_format_new();

    if (argc == 2 && 0 == strcmp(argv[1], "--test")) {
        logprint_run_tests();
        exit(0);
    }

    if (argc == 2 && 0 == strcmp(argv[1], "--help")) {
        android::show_help(argv[0]);
        exit(0);
    }

    for (;;) {
        int ret;

        ret = getopt(argc, argv, "cdgsQf:r::n:v:b:B");

        if (ret < 0) {
            break;
        }

        switch(ret) {
            case 's': 
                // default to all silent
                android_log_addFilterRule(g_logformat, "*:s");
            break;

            case 'c':
                clearLog = 1;
                mode = O_WRONLY;
            break;

            case 'd':
                mode |= O_NONBLOCK;
            break;

            case 'g':
                getLogSize = 1;
            break;

            case 'b':
                free(log_device);
                log_device =
                    (char*) malloc(strlen(LOG_FILE_DIR) + strlen(optarg) + 1);
                strcpy(log_device, LOG_FILE_DIR);
                strcat(log_device, optarg);

                android::g_isBinary = (strcmp(optarg, "events") == 0);
            break;

            case 'B':
                android::g_printBinary = 1;
            break;

            case 'f':
                // redirect output to a file

                android::g_outputFileName = optarg;

            break;

            case 'r':
                if (optarg == NULL) {                
                    android::g_logRotateSizeKBytes 
                                = DEFAULT_LOG_ROTATE_SIZE_KBYTES;
                } else {
                    long logRotateSize;
                    char *lastDigit;

                    if (!isdigit(optarg[0])) {
                        fprintf(stderr,"Invalid parameter to -r\n");
                        android::show_help(argv[0]);
                        exit(-1);
                    }
                    android::g_logRotateSizeKBytes = atoi(optarg);
                }
            break;

            case 'n':
                if (!isdigit(optarg[0])) {
                    fprintf(stderr,"Invalid parameter to -r\n");
                    android::show_help(argv[0]);
                    exit(-1);
                }

                android::g_maxRotatedLogs = atoi(optarg);
            break;

            case 'v':
                err = setLogFormat (optarg);
                if (err < 0) {
                    fprintf(stderr,"Invalid parameter to -v\n");
                    android::show_help(argv[0]);
                    exit(-1);
                }

                hasSetLogFormat = 1;
            break;

            case 'Q':
                /* this is a *hidden* option used to start a version of logcat                 */
                /* in an emulated device only. it basically looks for androidboot.logcat=      */
                /* on the kernel command line. If something is found, it extracts a log filter */
                /* and uses it to run the program. If nothing is found, the program should     */
                /* quit immediately                                                            */
#define  KERNEL_OPTION  "androidboot.logcat="
#define  CONSOLE_OPTION "androidboot.console="
                {
                    int          fd;
                    char*        logcat;
                    char*        console;
                    int          force_exit = 1;
                    static char  cmdline[1024];

                    fd = open("/proc/cmdline", O_RDONLY);
                    if (fd >= 0) {
                        int  n = read(fd, cmdline, sizeof(cmdline)-1 );
                        if (n < 0) n = 0;
                        cmdline[n] = 0;
                        close(fd);
                    } else {
                        cmdline[0] = 0;
                    }

                    logcat  = strstr( cmdline, KERNEL_OPTION );
                    console = strstr( cmdline, CONSOLE_OPTION );
                    if (logcat != NULL) {
                        char*  p = logcat + sizeof(KERNEL_OPTION)-1;;
                        char*  q = strpbrk( p, " \t\n\r" );;

                        if (q != NULL)
                            *q = 0;

                        forceFilters = p;
                        force_exit   = 0;
                    }
                    /* if nothing found or invalid filters, exit quietly */
                    if (force_exit)
                        exit(0);

                    /* redirect our output to the emulator console */
                    if (console) {
                        char*  p = console + sizeof(CONSOLE_OPTION)-1;
                        char*  q = strpbrk( p, " \t\n\r" );
                        char   devname[64];
                        int    len;

                        if (q != NULL) {
                            len = q - p;
                        } else
                            len = strlen(p);

                        len = snprintf( devname, sizeof(devname), "/dev/%.*s", len, p );
                        fprintf(stderr, "logcat using %s (%d)\n", devname, len);
                        if (len < (int)sizeof(devname)) {
                            fd = open( devname, O_WRONLY );
                            if (fd >= 0) {
                                dup2(fd, 1);
                                dup2(fd, 2);
                                close(fd);
                            }
                        }
                    }
                }
                break;

            default:
                fprintf(stderr,"Unrecognized Option\n");
                android::show_help(argv[0]);
                exit(-1);
            break;
        }
    }

    if (android::g_logRotateSizeKBytes != 0 
        && android::g_outputFileName == NULL
    ) {
        fprintf(stderr,"-r requires -f as well\n");
        android::show_help(argv[0]);
        exit(-1);
    }

    android::setupOutput();

    if (hasSetLogFormat == 0) {
        const char* logFormat = getenv("ANDROID_PRINTF_LOG");

        if (logFormat != NULL) {
            err = setLogFormat(logFormat);

            if (err < 0) {
                fprintf(stderr, "invalid format in ANDROID_PRINTF_LOG '%s'\n", 
                                    logFormat);
            }
        }
    }

    if (forceFilters) {
        err = android_log_addFilterString(g_logformat, forceFilters);
        if (err < 0) {
            fprintf (stderr, "Invalid filter expression in -logcat option\n");
            exit(0);
        }
    } else if (argc == optind) {
        // Add from environment variable
        char *env_tags_orig = getenv("ANDROID_LOG_TAGS");

        if (env_tags_orig != NULL) {
            err = android_log_addFilterString(g_logformat, env_tags_orig);

            if (err < 0) { 
                fprintf(stderr, "Invalid filter expression in" 
                                    " ANDROID_LOG_TAGS\n");
                android::show_help(argv[0]);
                exit(-1);
            }
        }
    } else {
        // Add from commandline
        for (int i = optind ; i < argc ; i++) {
            err = android_log_addFilterString(g_logformat, argv[i]);

            if (err < 0) { 
                fprintf (stderr, "Invalid filter expression '%s'\n", argv[i]);
                android::show_help(argv[0]);
                exit(-1);
            }
        }
    }

    logfd = open(log_device, mode);
    if (logfd < 0) {
        fprintf(stderr, "Unable to open log device '%s': %s\n",
            log_device, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (clearLog) {
        int ret;
        ret = android::clearLog(logfd);
        if (ret) {
            perror("ioctl");
            exit(EXIT_FAILURE);
        }
        return 0;
    }

    if (getLogSize) {
        int size, readable;

        size = android::getLogSize(logfd);
        if (size < 0) {
            perror("ioctl");
            exit(EXIT_FAILURE);
        }

        readable = android::getLogReadableSize(logfd);
        if (readable < 0) {
            perror("ioctl");
            exit(EXIT_FAILURE);
        }

        printf("ring buffer is %dKb (%dKb consumed), "
               "max entry is %db, max payload is %db\n",
               size / 1024, readable / 1024,
               (int) LOGGER_ENTRY_MAX_LEN, (int) LOGGER_ENTRY_MAX_PAYLOAD);
        return 0;
    }

    //LOG_EVENT_INT(10, 12345);
    //LOG_EVENT_LONG(11, 0x1122334455667788LL);
    //LOG_EVENT_STRING(0, "whassup, doc?");

    if (android::g_isBinary)
        android::g_eventTagMap = android_openEventTagMap(EVENT_TAG_MAP_FILE);

    android::readLogLines(logfd);

    return 0;
}
